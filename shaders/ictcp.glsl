// ITU-R BT.2100 ICtCp helpers (linear BT.709 RGB in/out for layer working space)
// GLSL mat3 constructors are COLUMN-major: each triplet is one column.
// Dolby ICtCp LMS (not HPE); LMS_TO_XYZ is the true inverse of XYZ_TO_LMS.

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

// XYZ -> Dolby LMS (ICtCp / BT.2100)
const mat3 ICTCP_XYZ_TO_LMS = mat3(
    0.3593, -0.1921, 0.0071,
    0.6976, 1.1005, 0.0748,
    -0.0359, 0.0754, 0.8433);

// True inverse of ICTCP_XYZ_TO_LMS
const mat3 ICTCP_LMS_TO_XYZ = mat3(
    2.0700345211, 0.3647497845, -0.0497812510,
    -1.3262311820, 0.6805456704, -0.0491978830,
    0.2067023247, -0.0453203205, 1.1880972056);

// LMS_PQ -> ICtCp (BT.2100 exact 4096 fractions)
const mat3 ICTCP_LMS_TO_ICTCP = mat3(
    2048.0 / 4096.0, 6610.0 / 4096.0, 17933.0 / 4096.0,
    2048.0 / 4096.0, -13613.0 / 4096.0, -17390.0 / 4096.0,
    0.0, 7003.0 / 4096.0, -543.0 / 4096.0);

const mat3 ICTCP_ICTCP_TO_LMS = mat3(
    1.0, 1.0, 1.0,
    0.00860903703793, -0.00860903703793, 0.56003133571068,
    0.11102962500303, -0.11102962500303, -0.32062717498732);

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
    // Do not clip Rec.709 negatives here — out-of-gamut values may become valid in BT.2020 at encode.
    return ICTCP_XYZ_TO_RGB * xyz * 10000.0;
}

#endif
