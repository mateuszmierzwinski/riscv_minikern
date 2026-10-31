/* A framebuffer's geometry, and the fbdev structs that describe it.
 *
 * The mode comes from the command line, so the parser is the untrusted
 * boundary: a malformed or oversized value must be refused, never turned
 * into an allocation size. The structs are ABI, so a field read at the
 * wrong offset would be a program drawing with the wrong stride — which is
 * a picture sheared diagonally, not an error.
 */
#include "core/fbinfo.h"

#include "core/errno.h"
#include "ktest.h"

static int parse(const char *text, struct fb_mode *m) {
    return fbinfo_parse_mode(text, strlen(text), m);
}

static void test_a_mode_is_read_with_its_stride(void) {
    struct fb_mode m;
    KTEST_CHECK(parse("1280x720", &m) == KERR_OK);
    KTEST_CHECK(m.width == 1280u && m.height == 720u);
    KTEST_CHECK(m.stride == 5120u);
    KTEST_CHECK(fbinfo_bytes(&m) == 3686400u);
}

/* An odd width still starts every line on a 64-byte boundary, and the size
 * covers whole pages. */
static void test_an_odd_width_pads_the_line(void) {
    struct fb_mode m;
    KTEST_CHECK(parse("17x16", &m) == KERR_OK);
    KTEST_CHECK(m.stride == 128u);
    KTEST_CHECK(fbinfo_bytes(&m) == 4096u);
}

static void test_malformed_modes_are_refused(void) {
    static const char *const bad[] = {"",       "1280",      "1280x",     "x720",  "1280*720",
                                      "1280x72a", "-1280x720", "1280x720 ", "8x8",  "4097x16",
                                      "16x4097",  "99999999999999999999x16", "1280xx720"};
    struct fb_mode m = {0};
    for (uint32_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        if (parse(bad[i], &m) != KERR_INVAL) {
            fprintf(stderr, "accepted \"%s\"\n", bad[i]);
            KTEST_CHECK(0);
        }
    }
    KTEST_CHECK(fbinfo_parse_mode(NULL, 4, &m) == KERR_INVAL);
}

/* A length shorter than the string ends the value there: the command line
 * is not NUL-terminated at the option's end. */
static void test_the_length_bounds_the_value(void) {
    struct fb_mode m;
    KTEST_CHECK(fbinfo_parse_mode("640x480 minikern.next", 7, &m) == KERR_OK);
    KTEST_CHECK(m.width == 640u && m.height == 480u);
}

static void test_the_variable_info_is_xrgb8888(void) {
    struct fb_mode m;
    struct fb_var_screeninfo v;
    (void)parse("640x480", &m);
    fbinfo_describe_var(&m, &v);
    KTEST_CHECK(v.xres == 640u && v.yres_virtual == 480u && v.bits_per_pixel == 32u);
    KTEST_CHECK(v.red.offset == 16u && v.green.offset == 8u && v.blue.offset == 0u);
    KTEST_CHECK(v.red.length == 8u && v.transp.length == 0u);
}

static void test_the_fixed_info_hides_the_physical_address(void) {
    struct fb_mode m;
    struct fb_fix_screeninfo f;
    (void)parse("640x480", &m);
    fbinfo_describe_fix(&m, "a name longer than sixteen", &f);
    KTEST_CHECK(f.smem_start == 0u);
    KTEST_CHECK(f.line_length == 2560u && f.smem_len == 1228800u);
    KTEST_CHECK(f.id[15] == '\0' && f.id[0] == 'a');
}

/* What `FBIOPUT_VSCREENINFO` may ask for: exactly what there is. */
static void test_only_the_current_mode_matches(void) {
    struct fb_mode m;
    struct fb_var_screeninfo v;
    (void)parse("640x480", &m);
    fbinfo_describe_var(&m, &v);
    KTEST_CHECK(fbinfo_var_matches(&m, &v));
    v.xres = 800u;
    KTEST_CHECK(!fbinfo_var_matches(&m, &v));
    fbinfo_describe_var(&m, &v);
    v.bits_per_pixel = 16u;
    KTEST_CHECK(!fbinfo_var_matches(&m, &v));
    fbinfo_describe_var(&m, &v);
    v.yoffset = 1u;
    KTEST_CHECK(!fbinfo_var_matches(&m, &v));
}

int main(void) {
    KTEST_RUN(test_a_mode_is_read_with_its_stride);
    KTEST_RUN(test_an_odd_width_pads_the_line);
    KTEST_RUN(test_malformed_modes_are_refused);
    KTEST_RUN(test_the_length_bounds_the_value);
    KTEST_RUN(test_the_variable_info_is_xrgb8888);
    KTEST_RUN(test_the_fixed_info_hides_the_physical_address);
    KTEST_RUN(test_only_the_current_mode_matches);
    return ktest_failures();
}
