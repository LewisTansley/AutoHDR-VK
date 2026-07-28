// ITU-R BT.2100 ICtCp helpers (linear BT.709 RGB in/out for layer working space)
// GLSL mat3 constructors are COLUMN-major: each triplet is one column.

#ifndef ICTCP_GLSL
#define ICTCP_GLSL

// BT.709 RGB -> XYZ (columns = matrix columns)
const mat3 ICTCP_RGB_TO_XYZ = mat3(
    0.4123908, 0.2126390, 0.0193308,
    0.3575843, 0.7151687, 0.1191948,
    0.1804808, 0.0721923, 0.9505322);

const mat3 ICTCP_XYZ_TO_RGB = mat3(
    3.2409699, -0.9692436, 0.0556301,
    -1.5373832, 1.8759675, -0.2039770,
    -0.4986108, 0.0415551, 1.0569715);

const mat3 ICTCP_BT2020_TO_XYZ = mat3(
    0.6369580, 0.2627002, 0.0000000,
    0.1446169, 0.6779981, 0.0280727,
    0.1688559, 0.0593017, 1.0609851);

// XYZ -> LMS (Hunt-Pointer-Estevez variant used with ICtCp)
const mat3 ICTCP_XYZ_TO_LMS = mat3(
    0.3592, -0.1922, 0.0070,
    0.6976, 1.1004, 0.0749,
    -0.0358, 0.0755, 0.8434);

const mat3 ICTCP_LMS_TO_XYZ = mat3(
    2.0701801, 0.3649882, -0.0496855,
    -1.3264565, 0.6805696, -0.0493859,
    0.2066160, -0.0453025, 1.1889440);

// LMS_PQ -> ICtCp (BT.2100)
const mat3 ICTCP_LMS_TO_ICTCP = mat3(
    0.5, 1.6137695, 4.3780623,
    0.5, -3.3234862, -4.2455399,
    0.0, 1.7097167, -0.1325225);

const mat3 ICTCP_ICTCP_TO_LMS = mat3(
    1.0, 1.0, 1.0,
    0.0086090, -0.0086090, 0.5600313,
    0.1110296, -0.1110296, -0.3206271);

float ictcpPqEotf(float v)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float vp = pow(max(v, 0.0), 1.0 / m2);
    float num = max(vp - c1, 0.0);
    float den = c2 - c3 * vp;
    return pow(num / max(den, 1e-6), 1.0 / m1);
}

float ictcpPqInvEotf(float y)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float yp = pow(max(y, 0.0), m1);
    float num = c1 + c2 * yp;
    float den = 1.0 + c3 * yp;
    return pow(num / max(den, 1e-6), m2);
}

vec3 linearRgbToICtCp(vec3 rgbNits)
{
    vec3 xyz = ICTCP_RGB_TO_XYZ * (rgbNits / 10000.0);
    vec3 lms = ICTCP_XYZ_TO_LMS * xyz;
    vec3 lmsPq = vec3(ictcpPqInvEotf(max(lms.x, 0.0)), ictcpPqInvEotf(max(lms.y, 0.0)),
                      ictcpPqInvEotf(max(lms.z, 0.0)));
    return ICTCP_LMS_TO_ICTCP * lmsPq;
}

vec3 iCtCpToLinearRgb(vec3 ictcp)
{
    vec3 lmsPq = ICTCP_ICTCP_TO_LMS * ictcp;
    vec3 lms = vec3(ictcpPqEotf(lmsPq.x), ictcpPqEotf(lmsPq.y), ictcpPqEotf(lmsPq.z));
    vec3 xyz = ICTCP_LMS_TO_XYZ * lms;
    return max(ICTCP_XYZ_TO_RGB * xyz * 10000.0, vec3(0.0));
}

#endif
