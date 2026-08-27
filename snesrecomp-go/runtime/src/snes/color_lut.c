#include "color_lut.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct Chromaticity { double x, y; } Chromaticity;
typedef struct ColorPrimaries {
    Chromaticity red, green, blue, white;
} ColorPrimaries;
typedef struct Matrix3 { double value[3][3]; } Matrix3;

static const ColorPrimaries kSrgb = {
    {0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}, {0.3127, 0.3290}
};
static const ColorPrimaries kSmpteC = {
    {0.630, 0.340}, {0.310, 0.595}, {0.155, 0.070}, {0.3127, 0.3290}
};
static const ColorPrimaries kTrinitron = {
    {0.621, 0.340}, {0.281, 0.606}, {0.152, 0.067}, {0.3127, 0.3290}
};

static uint32_t s_lut[32768];
static int s_active;

static void multiply_vector(const Matrix3 *matrix, const double input[3],
                            double output[3]) {
    for (unsigned row = 0; row < 3u; ++row) {
        output[row] = matrix->value[row][0] * input[0] +
                      matrix->value[row][1] * input[1] +
                      matrix->value[row][2] * input[2];
    }
}

static Matrix3 multiply_matrix(const Matrix3 *left, const Matrix3 *right) {
    Matrix3 output = {{{0}}};
    for (unsigned row = 0; row < 3u; ++row) {
        for (unsigned column = 0; column < 3u; ++column) {
            for (unsigned inner = 0; inner < 3u; ++inner) {
                output.value[row][column] +=
                    left->value[row][inner] * right->value[inner][column];
            }
        }
    }
    return output;
}

static Matrix3 inverse_matrix(const Matrix3 *matrix) {
    const double (*m)[3] = matrix->value;
    const double determinant =
        m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
        m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
        m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    Matrix3 output = {{
        {(m[1][1] * m[2][2] - m[1][2] * m[2][1]) / determinant,
         (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / determinant,
         (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / determinant},
        {(m[1][2] * m[2][0] - m[1][0] * m[2][2]) / determinant,
         (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / determinant,
         (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / determinant},
        {(m[1][0] * m[2][1] - m[1][1] * m[2][0]) / determinant,
         (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / determinant,
         (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / determinant}
    }};
    return output;
}

static void chromaticity_to_xyz(Chromaticity color, double xyz[3]) {
    xyz[0] = color.x / color.y;
    xyz[1] = 1.0;
    xyz[2] = (1.0 - color.x - color.y) / color.y;
}

static Matrix3 rgb_to_xyz(const ColorPrimaries *primaries) {
    double red[3], green[3], blue[3], white[3];
    chromaticity_to_xyz(primaries->red, red);
    chromaticity_to_xyz(primaries->green, green);
    chromaticity_to_xyz(primaries->blue, blue);
    chromaticity_to_xyz(primaries->white, white);
    Matrix3 basis = {{{red[0], green[0], blue[0]},
                      {red[1], green[1], blue[1]},
                      {red[2], green[2], blue[2]}}};
    const Matrix3 inverse = inverse_matrix(&basis);
    double scale[3];
    multiply_vector(&inverse, white, scale);
    for (unsigned row = 0; row < 3u; ++row) {
        for (unsigned column = 0; column < 3u; ++column) {
            basis.value[row][column] *= scale[column];
        }
    }
    return basis;
}

static Matrix3 conversion_matrix(const ColorPrimaries *source,
                                 const ColorPrimaries *destination) {
    const Matrix3 source_to_xyz = rgb_to_xyz(source);
    const Matrix3 destination_to_xyz = rgb_to_xyz(destination);
    const Matrix3 xyz_to_destination = inverse_matrix(&destination_to_xyz);
    return multiply_matrix(&xyz_to_destination, &source_to_xyz);
}

static double srgb_encode(double linear) {
    if (linear <= 0.0) return 0.0;
    if (linear >= 1.0) return 1.0;
    if (linear <= 0.0031308) return linear * 12.92;
    return 1.055 * pow(linear, 1.0 / 2.4) - 0.055;
}

static uint8_t quantize(double value) {
    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;
    return (uint8_t)(value * 255.0 + 0.5);
}

static void build_lut(const ColorPrimaries *panel) {
    const Matrix3 transform = conversion_matrix(panel, &kSrgb);
    for (unsigned pixel = 0; pixel < 32768u; ++pixel) {
        const double encoded[3] = {
            (double)(pixel & 31u) / 31.0,
            (double)((pixel >> 5) & 31u) / 31.0,
            (double)((pixel >> 10) & 31u) / 31.0
        };
        double linear[3];
        for (unsigned side = 0; side < 3u; ++side) {
            linear[side] = pow(encoded[side], 2.2);
        }
        double converted[3];
        multiply_vector(&transform, linear, converted);
        const uint8_t red = quantize(srgb_encode(converted[0]));
        const uint8_t green = quantize(srgb_encode(converted[1]));
        const uint8_t blue = quantize(srgb_encode(converted[2]));
        s_lut[pixel] = (uint32_t)red << 16 |
                       (uint32_t)green << 8 | blue;
    }
}

int snes_color_lut_configure(SnesColorModel model) {
    s_active = 0;
    switch (model) {
        case kSnesColorModelRaw: return 0;
        case kSnesColorModelCrt: build_lut(&kSmpteC); break;
        case kSnesColorModelTrinitron: build_lut(&kTrinitron); break;
        default: return 0;
    }
    s_active = 1;
    return 1;
}

int snes_color_lut_setup(void) {
    const char *setting = getenv("SNESRECOMP_SCREEN");
    if (setting == NULL || setting[0] == '\0' || strcmp(setting, "raw") == 0) {
        return snes_color_lut_configure(kSnesColorModelRaw);
    }
    if (strcmp(setting, "crt") == 0) {
        return snes_color_lut_configure(kSnesColorModelCrt);
    }
    if (strcmp(setting, "trinitron") == 0) {
        return snes_color_lut_configure(kSnesColorModelTrinitron);
    }
    return snes_color_lut_configure(kSnesColorModelRaw);
}

int snes_color_lut_active(void) {
    return s_active;
}

void snes_color_lut_map(const uint32_t *source, uint32_t *destination,
                        size_t pixel_count) {
    if (source == NULL || destination == NULL) return;
    if (!s_active) {
        if (source != destination) {
            memmove(destination, source, pixel_count * sizeof(*source));
        }
        return;
    }
    for (size_t index = 0; index < pixel_count; ++index) {
        const uint32_t color = source[index];
        const unsigned lut_index = ((color >> 19) & 31u) |
                                   ((color >> 11) & 31u) << 5 |
                                   ((color >> 3) & 31u) << 10;
        destination[index] = (color & 0xff000000u) | s_lut[lut_index];
    }
}
