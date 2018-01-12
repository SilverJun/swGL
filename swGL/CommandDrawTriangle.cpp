﻿#include <algorithm>
#include <cmath>
#include "DrawThread.h"
#include "ContextTypes.h"
#include "SIMD.h"
#include "OpenGL.h"
#include "Triangle.h"
#include "TextureManager.h"
#include "CommandDrawTriangle.h"

#define GRADIENT_VALUE(NAME) \
    qV ## NAME

#define GRADIENT_DX(NAME) \
    qDX ## NAME

#define GRADIENT_DY(NAME) \
    qDY ## NAME

#define DEFINE_GRADIENT(NAME) \
    QFloat GRADIENT_VALUE(NAME), GRADIENT_DX(NAME), GRADIENT_DY(NAME)

#define SETUP_GRADIENT_EQ(NAME, Q1, Q2, Q3) \
    setupGradientEquation(GRADIENT_VALUE(NAME), GRADIENT_DX(NAME), GRADIENT_DY(NAME), Q1, Q2, Q3, v1.x(), v1.y(), fdx21, fdy21, fdx31, fdy31, rcpArea)

#define GET_GRADIENT_VALUE_AFFINE(NAME) \
    _mm_add_ps(qV ## NAME, _mm_add_ps(_mm_mul_ps(xxxx, GRADIENT_DX(NAME)), _mm_mul_ps(yyyy, GRADIENT_DY(NAME))))

#define GET_GRADIENT_VALUE_PERSP(NAME) \
    _mm_mul_ps(w, GET_GRADIENT_VALUE_AFFINE(NAME))



//
// TODO: Find the cause of dropped fragments as there are cracks between adjacent triangles.
//       It is very obvious in Unreal Tournament's map CTF-LavaGiant.
//
namespace SWGL {

    static INLINED QInt getIntegerRGBA(ARGBColor &color) {

        const QFloat cMin = _mm_setzero_ps();
        const QFloat cMax = _mm_set1_ps(255.0f);

        // Scale floating point color values
        QFloat a = _mm_mul_ps(color.a, cMax);
        QFloat r = _mm_mul_ps(color.r, cMax);
        QFloat g = _mm_mul_ps(color.g, cMax);
        QFloat b = _mm_mul_ps(color.b, cMax);

        // Clamp the values between [0,255]
        a = SIMD::clamp(a, cMin, cMax);
        r = SIMD::clamp(r, cMin, cMax);
        g = SIMD::clamp(g, cMin, cMax);
        b = SIMD::clamp(b, cMin, cMax);

        // Build result
        QInt resA = _mm_slli_epi32(_mm_cvtps_epi32(a), 24);
        QInt resR = _mm_slli_epi32(_mm_cvtps_epi32(r), 16);
        QInt resG = _mm_slli_epi32(_mm_cvtps_epi32(g), 8);
        QInt resB = _mm_cvtps_epi32(b);

        return _mm_or_si128(
        
            _mm_or_si128(resA, resR),
            _mm_or_si128(resG, resB)
        );
    }

    static void setupGradientEquation(QFloat &qVAL, QFloat &qDQDX, QFloat &qDQDY, float q1, float q2, float q3, float x1, float y1, float dx21, float dy21, float dx31, float dy31, float rcpArea) {

        float dq21 = q2 - q1;
        float dq31 = q3 - q1;
        float dqdx = rcpArea * (dq21 * dy31 - dq31 * dy21);
        float dqdy = rcpArea * (dq31 * dx21 - dq21 * dx31);

        // Calculate the interpolant value at the origin point
        float value = q1 - (x1 * dqdx) - (y1 * dqdy);

        qDQDX = _mm_set1_ps(dqdx);
        qDQDY = _mm_set1_ps(dqdy);
        qVAL = _mm_set_ps(

            value + dqdy + dqdx,
            value + dqdy,
            value + dqdx,
            value
        );
    }

    static void setupEdgeEquation(QInt &eVAL, QInt &eDEDX, QInt &eDEDY, int x, int y, int dx, int dy, int minX, int minY, int width) {

        int dedx = -dy << 4;
        int dedy = dx << 4;
        int value = (dy * x) - (dx * y) + (dedx * minX) + (dedy * minY);

        if (dy < 0 || (dy == 0 && dx > 0)) {

            value++;
        }

        eDEDX = _mm_set1_epi32(dedx << 1);
        eDEDY = _mm_set1_epi32((dedy << 1) - (dedx * width));
        eVAL = _mm_set_epi32(

            value + dedy + dedx,
            value + dedy,
            value + dedx,
            value
        );
    }



    //
    // Draw triangle command
    //
    bool CommandDrawTriangle::execute(DrawThread *thread) {

        auto &drawBuffer = thread->getDrawBuffer();

        auto &scissor = m_state->scissor;
        auto &depthTesting = m_state->depthTesting;
        auto &blending = m_state->blending;
        auto &alphaTesting = m_state->alphaTesting;
        auto &polygonOffset = m_state->polygonOffset;
        auto &textureState = m_state->textures;
        auto &deferedDepthWrite = m_state->deferedDepthWrite;
        auto &colorMask = m_state->colorMask;

        for (auto triangleIdx : m_indices) {

            auto &t = m_state->triangles[triangleIdx];
            auto &v1 = t.v[0].raster;
            auto &v2 = t.v[1].raster;
            auto &v3 = t.v[2].raster;

            //
            // Calculate the triangles reciprocal area
            //
            float rcpArea = 1.0f / ((v2.x() - v1.x()) * (v3.y() - v1.y()) -
                                    (v2.y() - v1.y()) * (v3.x() - v1.x()));

            //
            // Calculate fixed point coordinates
            //
            int x1, y1, x2, y2, x3, y3;

            x1 = static_cast<int>(v1.x() * 16.0f);
            y1 = static_cast<int>(v1.y() * 16.0f);
            if (rcpArea < 0.0f) {

                x2 = static_cast<int>(v2.x() * 16.0f);
                y2 = static_cast<int>(v2.y() * 16.0f);
                x3 = static_cast<int>(v3.x() * 16.0f);
                y3 = static_cast<int>(v3.y() * 16.0f);
            }
            else {

                x2 = static_cast<int>(v3.x() * 16.0f);
                y2 = static_cast<int>(v3.y() * 16.0f);
                x3 = static_cast<int>(v2.x() * 16.0f);
                y3 = static_cast<int>(v2.y() * 16.0f);
            }

            //
            // Determine triangle bounding box with respect to our rendertarget
            //
            int minY = std::max((std::min({ y1, y2, y3 }) + 0x0f) >> 4, drawBuffer.getMinY());
            int maxY = std::min((std::max({ y1, y2, y3 }) + 0x0f) >> 4, drawBuffer.getMaxY());
            int minX = std::max((std::min({ x1, x2, x3 }) + 0x0f) >> 4, drawBuffer.getMinX());
            int maxX = std::min((std::max({ x1, x2, x3 }) + 0x0f) >> 4, drawBuffer.getMaxX());

            if (scissor.isEnabled()) {
            
                scissor.cut(minX, minY, maxX, maxY);
            }
            

            // Make sure that we rasterize at the beginning of a quad (which is 2x2 pixel).
            // Then determine the width of the bounding box in full quads.
            minX &= ~1;
            minY &= ~1;

            int width = (1 + (maxX - minX)) & ~1;

            // Determine the write position into the color and depth buffer
            int startX = minX - drawBuffer.getMinX();
            int startY = minY - drawBuffer.getMinY();

            size_t bufferOffset = (startX << 1) + (startY * drawBuffer.getWidth());
            size_t bufferStride = (drawBuffer.getWidth() - width) << 1;

            unsigned int *colorBuffer = drawBuffer.getColor() + bufferOffset;
            float *depthBuffer = drawBuffer.getDepth() + bufferOffset;

            //
            // Determine the triangle edge equations
            //
            int dx12 = x1 - x2, dx23 = x2 - x3, dx31 = x3 - x1;
            int dy12 = y1 - y2, dy23 = y2 - y3, dy31 = y3 - y1;

            QInt edgeValue[3], edgeDX[3], edgeDY[3];
            setupEdgeEquation(edgeValue[0], edgeDX[0], edgeDY[0], x1, y1, dx12, dy12, minX, minY, width);
            setupEdgeEquation(edgeValue[1], edgeDX[1], edgeDY[1], x2, y2, dx23, dy23, minX, minY, width);
            setupEdgeEquation(edgeValue[2], edgeDX[2], edgeDY[2], x3, y3, dx31, dy31, minX, minY, width);

            //
            // Determine the gradient equations
            //
            float fdx21 = v2.x() - v1.x(), fdy21 = v2.y() - v1.y();
            float fdx31 = v3.x() - v1.x(), fdy31 = v3.y() - v1.y();

            DEFINE_GRADIENT(z);
            SETUP_GRADIENT_EQ(z, v1.z(), v2.z(), v3.z());

            DEFINE_GRADIENT(rcpW);
            SETUP_GRADIENT_EQ(rcpW, v1.w(), v2.w(), v3.w());

            DEFINE_GRADIENT(r);
            SETUP_GRADIENT_EQ(r, t.v[0].color.r(), t.v[1].color.r(), t.v[2].color.r());
            DEFINE_GRADIENT(g);
            SETUP_GRADIENT_EQ(g, t.v[0].color.g(), t.v[1].color.g(), t.v[2].color.g());
            DEFINE_GRADIENT(b);
            SETUP_GRADIENT_EQ(b, t.v[0].color.b(), t.v[1].color.b(), t.v[2].color.b());
            DEFINE_GRADIENT(a);
            SETUP_GRADIENT_EQ(a, t.v[0].color.a(), t.v[1].color.a(), t.v[2].color.a());

            DEFINE_GRADIENT(texS[SWGL_MAX_TEXTURE_UNITS]);
            DEFINE_GRADIENT(texT[SWGL_MAX_TEXTURE_UNITS]);
            DEFINE_GRADIENT(texR[SWGL_MAX_TEXTURE_UNITS]);
            DEFINE_GRADIENT(texQ[SWGL_MAX_TEXTURE_UNITS]);
            for (size_t i = 0; i < SWGL_MAX_TEXTURE_UNITS; i++) {

                SETUP_GRADIENT_EQ(texS[i], t.v[0].texCoord[i].x(), t.v[1].texCoord[i].x(), t.v[2].texCoord[i].x());
                SETUP_GRADIENT_EQ(texT[i], t.v[0].texCoord[i].y(), t.v[1].texCoord[i].y(), t.v[2].texCoord[i].y());
                SETUP_GRADIENT_EQ(texR[i], t.v[0].texCoord[i].z(), t.v[1].texCoord[i].z(), t.v[2].texCoord[i].z());
                SETUP_GRADIENT_EQ(texQ[i], t.v[0].texCoord[i].w(), t.v[1].texCoord[i].w(), t.v[2].texCoord[i].w());
            }

            //
            // Calculate polygon offset (see page 77, glspec13.pdf)
            //
            QFloat zOffset;

            if (polygonOffset.isFillEnabled()) {

                QFloat m = _mm_max_ps(
                
                    SIMD::absolute(GRADIENT_DX(z)),
                    SIMD::absolute(GRADIENT_DY(z))
                );
                zOffset = SIMD::multiplyAdd(
                
                    m,
                    _mm_set1_ps(polygonOffset.getFactor()),
                    _mm_set1_ps(polygonOffset.getRTimesUnits())
                );
            }
            else {

                zOffset = _mm_set1_ps(0.0f);
            }


            //
            // Rasterize and shade the triangle
            //
            ARGBColor srcColor;
            ARGBColor texColor;
            TextureCoordinates texCoords;

            for (int y = minY; y < maxY; y += 2) {

                QFloat yyyy = _mm_set1_ps(static_cast<float>(y));

                for (int x = minX; x < maxX; x += 2) {


                    //
                    // Coverage test for a 2x2 pixel quad
                    //
                    QInt e0 = _mm_cmpgt_epi32(edgeValue[0], _mm_setzero_si128());
                    QInt e1 = _mm_cmpgt_epi32(edgeValue[1], _mm_setzero_si128());
                    QInt e2 = _mm_cmpgt_epi32(edgeValue[2], _mm_setzero_si128());
                    QInt fragmentMask = _mm_and_si128(_mm_and_si128(e0, e1), e2);

                    if (_mm_testz_si128(fragmentMask, fragmentMask) == 0) {

                        QFloat xxxx = _mm_set1_ps(static_cast<float>(x));


                        //
                        // (Early) Depth test
                        //
                        QFloat depthBufferZ, currentZ;

                        if (depthTesting.isTestEnabled()) {

                            depthBufferZ = _mm_load_ps(depthBuffer);
                            currentZ = _mm_add_ps(zOffset, GET_GRADIENT_VALUE_AFFINE(z));

                            switch (depthTesting.getTestFunction()) {

                            case GL_LESS: fragmentMask = _mm_and_si128(fragmentMask, _mm_castps_si128(_mm_cmplt_ps(currentZ, depthBufferZ))); break;
                            case GL_LEQUAL: fragmentMask = _mm_and_si128(fragmentMask, _mm_castps_si128(_mm_cmple_ps(currentZ, depthBufferZ))); break;
                            case GL_GREATER: fragmentMask = _mm_and_si128(fragmentMask, _mm_castps_si128(_mm_cmpgt_ps(currentZ, depthBufferZ))); break;
                            case GL_GEQUAL: fragmentMask = _mm_and_si128(fragmentMask, _mm_castps_si128(_mm_cmpge_ps(currentZ, depthBufferZ))); break;
                            case GL_EQUAL: fragmentMask = _mm_and_si128(fragmentMask, _mm_castps_si128(_mm_cmpeq_ps(currentZ, depthBufferZ))); break;
                            case GL_NOTEQUAL: fragmentMask = _mm_and_si128(fragmentMask, _mm_castps_si128(_mm_cmpneq_ps(currentZ, depthBufferZ))); break;
                            case GL_ALWAYS: break;
                            case GL_NEVER: fragmentMask = _mm_setzero_si128(); break;
                            }

                            // Check if any fragment survived the depth test
                            if (_mm_testz_si128(fragmentMask, fragmentMask) != 0) {

                                goto nextQuad;
                            }

                            // Write the new depth values to the depth buffer
                            if (depthTesting.isWriteEnabled() && !deferedDepthWrite) {

                                _mm_store_ps(

                                    depthBuffer,
                                    SIMD::blend(depthBufferZ, currentZ, _mm_castsi128_ps(fragmentMask))
                                );
                            }
                        }


                        //
                        // Calculate perspective w
                        //
                        QFloat w = _mm_div_ps(_mm_set1_ps(1.0f), GET_GRADIENT_VALUE_AFFINE(rcpW));


                        //
                        // Set the fragments initial color
                        //
                        srcColor.a = GET_GRADIENT_VALUE_PERSP(a);
                        srcColor.r = GET_GRADIENT_VALUE_PERSP(r);
                        srcColor.g = GET_GRADIENT_VALUE_PERSP(g);
                        srcColor.b = GET_GRADIENT_VALUE_PERSP(b);


                        //
                        // Texture sampling and blending for each active texture unit
                        //
                        for (auto i = 0; i < SWGL_MAX_TEXTURE_UNITS; i++) {

                            auto &texState = textureState[i];
                            if (texState.texObj == nullptr) {

                                continue;
                            }

                            // Get texture sample
                            texCoords.s = GET_GRADIENT_VALUE_PERSP(texS[i]);
                            texCoords.t = GET_GRADIENT_VALUE_PERSP(texT[i]);
                            texCoords.r = GET_GRADIENT_VALUE_PERSP(texR[i]);
                            texCoords.q = GET_GRADIENT_VALUE_PERSP(texQ[i]);
                            sampleTexels(texState.texObj, texState.texParams, texCoords, texColor);

                            // Execute the texturing function
                            switch (texState.texEnv.mode) {

                            case GL_REPLACE:
                                switch (texState.texObj->format) {

                                case TextureBaseFormat::Alpha:
                                    srcColor.a = texColor.a;
                                    break;

                                case TextureBaseFormat::RGB:
                                case TextureBaseFormat::Luminance:
                                    srcColor.r = texColor.r;
                                    srcColor.g = texColor.g;
                                    srcColor.b = texColor.b;
                                    break;

                                case TextureBaseFormat::LuminanceAlpha:
                                case TextureBaseFormat::Intensity:
                                case TextureBaseFormat::RGBA:
                                    srcColor.a = texColor.a;
                                    srcColor.r = texColor.r;
                                    srcColor.g = texColor.g;
                                    srcColor.b = texColor.b;
                                    break;
                                }
                                break;

                            case GL_MODULATE:
                                switch (texState.texObj->format) {

                                case TextureBaseFormat::Alpha:
                                    srcColor.a = _mm_mul_ps(srcColor.a, texColor.a);
                                    break;
                                    
                                case TextureBaseFormat::LuminanceAlpha:
                                case TextureBaseFormat::Intensity:
                                case TextureBaseFormat::RGBA:
                                    srcColor.a = _mm_mul_ps(srcColor.a, texColor.a);
                                case TextureBaseFormat::Luminance:
                                case TextureBaseFormat::RGB:
                                    srcColor.r = _mm_mul_ps(srcColor.r, texColor.r);
                                    srcColor.g = _mm_mul_ps(srcColor.g, texColor.g);
                                    srcColor.b = _mm_mul_ps(srcColor.b, texColor.b);
                                    break;
                                }
                                break;

                            case GL_DECAL:
                                switch (texState.texObj->format) {

                                case TextureBaseFormat::RGB:
                                    srcColor.r = texColor.r;
                                    srcColor.g = texColor.g;
                                    srcColor.b = texColor.b;
                                    break;

                                case TextureBaseFormat::RGBA:
                                    srcColor.r = _mm_add_ps(_mm_mul_ps(srcColor.r, _mm_sub_ps(_mm_set1_ps(1.0f), texColor.a)), _mm_mul_ps(texColor.r, texColor.a));
                                    srcColor.g = _mm_add_ps(_mm_mul_ps(srcColor.g, _mm_sub_ps(_mm_set1_ps(1.0f), texColor.a)), _mm_mul_ps(texColor.g, texColor.a));
                                    srcColor.b = _mm_add_ps(_mm_mul_ps(srcColor.b, _mm_sub_ps(_mm_set1_ps(1.0f), texColor.a)), _mm_mul_ps(texColor.b, texColor.a));
                                    break;
                                }
                                break;

                            case GL_ADD:
                                switch (texState.texObj->format) {

                                case TextureBaseFormat::Alpha:
                                    srcColor.a = _mm_mul_ps(srcColor.a, texColor.a);
                                    break;

                                case TextureBaseFormat::LuminanceAlpha:
                                case TextureBaseFormat::RGBA:
                                    srcColor.a = _mm_mul_ps(srcColor.a, texColor.a);
                                case TextureBaseFormat::Luminance:
                                case TextureBaseFormat::RGB:
                                    srcColor.r = _mm_min_ps(_mm_set1_ps(1.0f), _mm_add_ps(srcColor.r, texColor.r));
                                    srcColor.g = _mm_min_ps(_mm_set1_ps(1.0f), _mm_add_ps(srcColor.g, texColor.g));
                                    srcColor.b = _mm_min_ps(_mm_set1_ps(1.0f), _mm_add_ps(srcColor.b, texColor.b));
                                    break;

                                case TextureBaseFormat::Intensity:
                                    srcColor.a = _mm_min_ps(_mm_set1_ps(1.0f), _mm_add_ps(srcColor.a, texColor.a));
                                    srcColor.r = _mm_min_ps(_mm_set1_ps(1.0f), _mm_add_ps(srcColor.r, texColor.r));
                                    srcColor.g = _mm_min_ps(_mm_set1_ps(1.0f), _mm_add_ps(srcColor.g, texColor.g));
                                    srcColor.b = _mm_min_ps(_mm_set1_ps(1.0f), _mm_add_ps(srcColor.b, texColor.b));
                                    break;
                                }
                                break;

                            case GL_BLEND:
                                switch (texState.texObj->format) {

                                case TextureBaseFormat::Alpha:
                                    srcColor.a = _mm_mul_ps(srcColor.a, texColor.a);
                                    break;

                                case TextureBaseFormat::LuminanceAlpha:
                                case TextureBaseFormat::RGBA:
                                    srcColor.a = _mm_mul_ps(srcColor.a, texColor.a);
                                case TextureBaseFormat::Luminance:
                                case TextureBaseFormat::RGB:
                                    srcColor.r = SIMD::lerp(texColor.r, srcColor.r, _mm_set1_ps(texState.texEnv.colorConstR));
                                    srcColor.g = SIMD::lerp(texColor.g, srcColor.g, _mm_set1_ps(texState.texEnv.colorConstG));
                                    srcColor.b = SIMD::lerp(texColor.b, srcColor.b, _mm_set1_ps(texState.texEnv.colorConstB));
                                    break;

                                case TextureBaseFormat::Intensity:
                                    srcColor.a = SIMD::lerp(texColor.a, srcColor.a, _mm_set1_ps(texState.texEnv.colorConstA));
                                    srcColor.r = SIMD::lerp(texColor.r, srcColor.r, _mm_set1_ps(texState.texEnv.colorConstR));
                                    srcColor.g = SIMD::lerp(texColor.g, srcColor.g, _mm_set1_ps(texState.texEnv.colorConstG));
                                    srcColor.b = SIMD::lerp(texColor.b, srcColor.b, _mm_set1_ps(texState.texEnv.colorConstB));
                                    break;
                                }
                                break;

                            // TODO: Unimplemented
                            case GL_COMBINE:
                                srcColor.a = _mm_set1_ps(1.0f);
                                srcColor.r = _mm_set1_ps(1.0f);
                                srcColor.g = _mm_set1_ps(0.0f);
                                srcColor.b = _mm_set1_ps(1.0f);
                                break;
                            }
                        }

                        //
                        // Alpha testing
                        //
                        if (alphaTesting.isEnabled()) {

                            QFloat refVal = _mm_set1_ps(alphaTesting.getReferenceValue());

                            switch (alphaTesting.getTestFunction()) {

                            case GL_LESS: fragmentMask = _mm_and_si128(fragmentMask, _mm_castps_si128(_mm_cmplt_ps(srcColor.a, refVal))); break;
                            case GL_LEQUAL: fragmentMask = _mm_and_si128(fragmentMask, _mm_castps_si128(_mm_cmple_ps(srcColor.a, refVal))); break;
                            case GL_GREATER: fragmentMask = _mm_and_si128(fragmentMask, _mm_castps_si128(_mm_cmpgt_ps(srcColor.a, refVal))); break;
                            case GL_GEQUAL: fragmentMask = _mm_and_si128(fragmentMask, _mm_castps_si128(_mm_cmpge_ps(srcColor.a, refVal))); break;
                            case GL_EQUAL: fragmentMask = _mm_and_si128(fragmentMask, _mm_castps_si128(_mm_cmpeq_ps(srcColor.a, refVal))); break;
                            case GL_NOTEQUAL: fragmentMask = _mm_and_si128(fragmentMask, _mm_castps_si128(_mm_cmpneq_ps(srcColor.a, refVal))); break;
                            case GL_ALWAYS: break;
                            case GL_NEVER: fragmentMask = _mm_setzero_si128(); break;
                            }

                            // Check if any fragment survived the alpha test
                            if (_mm_testz_si128(fragmentMask, fragmentMask) != 0) {

                                goto nextQuad;
                            }

                            // The write to the depthbuffer can be defered after alpha testing is done. This makes
                            // it possible to do a early depthbuffer test while maintaining the "natural" flow of
                            // data as OpenGL specifies it.
                            if (deferedDepthWrite) {

                                _mm_store_ps(

                                    depthBuffer,
                                    SIMD::blend(depthBufferZ, currentZ, _mm_castsi128_ps(fragmentMask))
                                );
                            }
                        }


                        //
                        // Blending with the color buffer
                        //
                        QInt quadBackbuffer = _mm_load_si128(reinterpret_cast<QInt *>(colorBuffer));
                        QInt quadBlendingResult;

                        if (blending.isEnabled()) {

                            // Convert the backbuffer colors back to floats
                            const QFloat normalize = _mm_set1_ps(1.0f / 255.0f);
                            const QInt mask = _mm_set1_epi32(0xff);

                            ARGBColor dstColor = {

                                _mm_mul_ps(normalize, _mm_cvtepi32_ps(_mm_srli_epi32(quadBackbuffer, 24))),
                                _mm_mul_ps(normalize, _mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(quadBackbuffer, 16), mask))),
                                _mm_mul_ps(normalize, _mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(quadBackbuffer, 8), mask))),
                                _mm_mul_ps(normalize, _mm_cvtepi32_ps(_mm_and_si128(quadBackbuffer, mask)))
                            };

                            // Determine the source and destination blending factors
                            QFloat srcFactorA, srcFactorR, srcFactorG, srcFactorB;
                            QFloat dstFactorA, dstFactorR, dstFactorG, dstFactorB;

                            switch (blending.getSourceFactor()) {

                            case GL_ZERO:
                                srcFactorA = _mm_setzero_ps();
                                srcFactorR = _mm_setzero_ps();
                                srcFactorG = _mm_setzero_ps();
                                srcFactorB = _mm_setzero_ps();
                                break;

                            case GL_ONE:
                                srcFactorA = _mm_set1_ps(1.0f);
                                srcFactorR = _mm_set1_ps(1.0f);
                                srcFactorG = _mm_set1_ps(1.0f);
                                srcFactorB = _mm_set1_ps(1.0f);
                                break;

                            case GL_SRC_COLOR:
                                srcFactorA = srcColor.a;
                                srcFactorR = srcColor.r;
                                srcFactorG = srcColor.g;
                                srcFactorB = srcColor.b;
                                break;

                            case GL_ONE_MINUS_SRC_COLOR:
                                srcFactorA = _mm_sub_ps(_mm_set1_ps(1.0f), srcColor.a);
                                srcFactorR = _mm_sub_ps(_mm_set1_ps(1.0f), srcColor.r);
                                srcFactorG = _mm_sub_ps(_mm_set1_ps(1.0f), srcColor.g);
                                srcFactorB = _mm_sub_ps(_mm_set1_ps(1.0f), srcColor.b);
                                break;

                            case GL_DST_COLOR:
                                srcFactorA = dstColor.a;
                                srcFactorR = dstColor.r;
                                srcFactorG = dstColor.g;
                                srcFactorB = dstColor.b;
                                break;

                            case GL_ONE_MINUS_DST_COLOR:
                                srcFactorA = _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.a);
                                srcFactorR = _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.r);
                                srcFactorG = _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.g);
                                srcFactorB = _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.b);
                                break;

                            case GL_SRC_ALPHA:
                                srcFactorA = srcColor.a;
                                srcFactorR = srcColor.a;
                                srcFactorG = srcColor.a;
                                srcFactorB = srcColor.a;
                                break;

                            case GL_ONE_MINUS_SRC_ALPHA:
                                srcFactorA = _mm_sub_ps(_mm_set1_ps(1.0f), srcColor.a);
                                srcFactorR = _mm_sub_ps(_mm_set1_ps(1.0f), srcColor.a);
                                srcFactorG = _mm_sub_ps(_mm_set1_ps(1.0f), srcColor.a);
                                srcFactorB = _mm_sub_ps(_mm_set1_ps(1.0f), srcColor.a);
                                break;

                            case GL_DST_ALPHA:
                                srcFactorA = dstColor.a;
                                srcFactorR = dstColor.a;
                                srcFactorG = dstColor.a;
                                srcFactorB = dstColor.a;
                                break;

                            case GL_ONE_MINUS_DST_ALPHA:
                                srcFactorA = _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.a);
                                srcFactorR = _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.a);
                                srcFactorG = _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.a);
                                srcFactorB = _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.a);
                                break;

                            case GL_SRC_ALPHA_SATURATE:
                                srcFactorA = _mm_set1_ps(1.0f);
                                srcFactorR = _mm_min_ps(srcColor.a, _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.a));
                                srcFactorG = _mm_min_ps(srcColor.a, _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.a));
                                srcFactorB = _mm_min_ps(srcColor.a, _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.a));
                                break;
                            }

                            switch (blending.getDestinationFactor()) {

                            case GL_ZERO:
                                dstFactorA = _mm_setzero_ps();
                                dstFactorR = _mm_setzero_ps();
                                dstFactorG = _mm_setzero_ps();
                                dstFactorB = _mm_setzero_ps();
                                break;

                            case GL_ONE:
                                dstFactorA = _mm_set1_ps(1.0f);
                                dstFactorR = _mm_set1_ps(1.0f);
                                dstFactorG = _mm_set1_ps(1.0f);
                                dstFactorB = _mm_set1_ps(1.0f);
                                break;

                            case GL_SRC_COLOR:
                                dstFactorA = srcColor.a;
                                dstFactorR = srcColor.r;
                                dstFactorG = srcColor.g;
                                dstFactorB = srcColor.b;
                                break;

                            case GL_ONE_MINUS_SRC_COLOR:
                                dstFactorA = _mm_sub_ps(_mm_set1_ps(1.0f), srcColor.a);
                                dstFactorR = _mm_sub_ps(_mm_set1_ps(1.0f), srcColor.r);
                                dstFactorG = _mm_sub_ps(_mm_set1_ps(1.0f), srcColor.g);
                                dstFactorB = _mm_sub_ps(_mm_set1_ps(1.0f), srcColor.b);
                                break;

                            case GL_DST_COLOR:
                                dstFactorA = dstColor.a;
                                dstFactorR = dstColor.r;
                                dstFactorG = dstColor.g;
                                dstFactorB = dstColor.b;
                                break;

                            case GL_ONE_MINUS_DST_COLOR:
                                dstFactorA = _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.a);
                                dstFactorR = _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.r);
                                dstFactorG = _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.g);
                                dstFactorB = _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.b);
                                break;

                            case GL_SRC_ALPHA:
                                dstFactorA = srcColor.a;
                                dstFactorR = srcColor.a;
                                dstFactorG = srcColor.a;
                                dstFactorB = srcColor.a;
                                break;

                            case GL_ONE_MINUS_SRC_ALPHA:
                                dstFactorA = _mm_sub_ps(_mm_set1_ps(1.0f), srcColor.a);
                                dstFactorR = _mm_sub_ps(_mm_set1_ps(1.0f), srcColor.a);
                                dstFactorG = _mm_sub_ps(_mm_set1_ps(1.0f), srcColor.a);
                                dstFactorB = _mm_sub_ps(_mm_set1_ps(1.0f), srcColor.a);
                                break;

                            case GL_DST_ALPHA:
                                dstFactorA = dstColor.a;
                                dstFactorR = dstColor.a;
                                dstFactorG = dstColor.a;
                                dstFactorB = dstColor.a;
                                break;

                            case GL_ONE_MINUS_DST_ALPHA:
                                dstFactorA = _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.a);
                                dstFactorR = _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.a);
                                dstFactorG = _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.a);
                                dstFactorB = _mm_sub_ps(_mm_set1_ps(1.0f), dstColor.a);
                                break;
                            }

                            // Perform the blending
                            srcColor.a = _mm_add_ps(_mm_mul_ps(srcColor.a, srcFactorA), _mm_mul_ps(dstColor.a, dstFactorA));
                            srcColor.r = _mm_add_ps(_mm_mul_ps(srcColor.r, srcFactorR), _mm_mul_ps(dstColor.r, dstFactorR));
                            srcColor.g = _mm_add_ps(_mm_mul_ps(srcColor.g, srcFactorG), _mm_mul_ps(dstColor.g, dstFactorG));
                            srcColor.b = _mm_add_ps(_mm_mul_ps(srcColor.b, srcFactorB), _mm_mul_ps(dstColor.b, dstFactorB));
                        }

                        quadBlendingResult = getIntegerRGBA(srcColor);


                        //
                        // Color masking
                        //
                        quadBlendingResult = SIMD::mask(

                            quadBlendingResult,
                            quadBackbuffer,
                            _mm_set1_epi32(colorMask.getMask())
                        );


                        //
                        // Store final color in the color buffer
                        //
                        _mm_store_si128(
                        
                            reinterpret_cast<QInt *>(colorBuffer),
                            SIMD::blend(quadBackbuffer, quadBlendingResult, fragmentMask)
                        );
                    }

                nextQuad:

                    // Update edge equation values with respect to the change in x
                    edgeValue[0] = _mm_add_epi32(edgeValue[0], edgeDX[0]);
                    edgeValue[1] = _mm_add_epi32(edgeValue[1], edgeDX[1]);
                    edgeValue[2] = _mm_add_epi32(edgeValue[2], edgeDX[2]);

                    // Update buffer address
                    colorBuffer += 4;
                    depthBuffer += 4;
                }

                // Update edge equation values with respect to the change in y
                edgeValue[0] = _mm_add_epi32(edgeValue[0], edgeDY[0]);
                edgeValue[1] = _mm_add_epi32(edgeValue[1], edgeDY[1]);
                edgeValue[2] = _mm_add_epi32(edgeValue[2], edgeDY[2]);

                // Update buffer address
                colorBuffer += bufferStride;
                depthBuffer += bufferStride;
            }
        }

        return true;
    }
}

#undef GET_GRADIENT_VALUE_PERSP
#undef GET_GRADIENT_VALUE_AFFINE
#undef SETUP_GRADIENT_EQ
#undef DEFINE_GRADIENT
#undef GRADIENT_DY
#undef GRADIENT_DX
#undef GRADIENT_VALUE
