#include "common.h"

uint32_t
air_read32(const volatile uint32_t *p)
{
    return *p;
}

void
air_read64(const volatile void *p)
{
    (void)*(const volatile uint64_t *)p;
}

void
air_write32(volatile uint32_t *p, uint32_t value)
{
    *p = value;
}

size_t
air_encode(uint8_t *out, const uint8_t *in, size_t n)
{
    size_t nb = 0;
    out[nb++] = 0;
    for (unsigned i = 0; i < 14; i++) {
        out[nb++] = (SYNC >> i) & 1;
    }
    for (unsigned i = 0; i < 8; i++) {
        out[nb++] = (n >> i) & 1;
    }
    for (size_t i = 0; i < n; i++) {
        for (unsigned j = 0; j < 8; j++) {
            out[nb++] = (in[i] >> j) & 1;
        }
    }
    return nb;
}


static void
sort_doubles(double *values, size_t count)
{
    for (size_t i = 1; i < count; i++) {
        double value = values[i];
        size_t j = i;
        while (j && values[j - 1] > value) {
            values[j] = values[j - 1];
            j--;
        }
        values[j] = value;
    }
}

double
air_median(double *values, size_t count)
{
    sort_doubles(values, count);
    return 0.5 * (values[(count - 1) / 2] + values[count / 2]);
}

uint32_t
air_ring(volatile uint32_t *ring, uint32_t *order, size_t lines, uint32_t start)
{
    for (size_t i = 0; i < lines; i++) {
        order[i] = i;
    }
    for (size_t i = lines - 1; i > 0; i--) {
        size_t j = (uint32_t)mix(i) % (i + 1);
        uint32_t tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
    for (size_t i = 0; i < lines; i++) {
        air_write32(&ring[order[i] * 16], order[(i + 1) % lines] * 16);
    }
    return start % lines * 16;
}
