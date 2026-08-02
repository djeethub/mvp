vec3 to_rgb(float y, float u, float v, int colorRange, int colorspace) {
    // 2. Adjust for Range Bias (AVColorRange)
    // AVCOL_RANGE_JPEG = 2 (Full Range [0, 255])
    // AVCOL_RANGE_MPEG = 1 (Limited/Broadcast Range [16, 235])
    if (colorRange == 1) { 
        y = (y - (16.0 / 255.0)) * (255.0 / 219.0);
        u = u - (128.0 / 255.0);
        v = v - (128.0 / 255.0);
    } else {
        // Full range still requires shifting Chroma components to center zero
        u = u - (128.0 / 255.0);
        v = v - (128.0 / 255.0);
    }

    vec3 yuv = vec3(y, u, v);
    vec3 rgb;

    // 3. Apply the dynamic transformation matrix based on AVColorSpace
    // Values match FFmpeg's enum definition entries natively
    if (colorspace == 1) { 
        // AVCOL_SPC_BT709 (HD Video)
        mat3 transform709 = transpose(mat3(
            1.0,  0.0,      1.5748,
            1.0, -0.1873,  -0.4681,
            1.0,  1.8556,   0.0
        ));
        rgb = transform709 * yuv;
    } 
    else if (colorspace == 9) { 
        // AVCOL_SPC_BT2020_NCL (4K / HDR Non-constant Luminance)
        mat3 transform2020 = transpose(mat3(
            1.0,  0.0,      1.4746,
            1.0, -0.1646,  -0.5714,
            1.0,  1.8814,   0.0
        ));
        rgb = transform2020 * yuv;
    } 
    else { 
        // Default / AVCOL_SPC_BT470BG or AVCOL_SPC_SMPTE170M (BT.601 SD Video)
        mat3 transform601 = transpose(mat3(
            1.0,  0.0,      1.402,
            1.0, -0.34414, -0.71414,
            1.0,  1.772,    0.0
        ));
        rgb = transform601 * yuv;
    }

    return rgb;
}
