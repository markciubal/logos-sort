/* sort_napi.c — Node-API bridge for logos-sort.
 *
 * Exports two functions to JavaScript:
 *
 *   sortFloat64Array(arr)
 *     Zero-copy native sort on a Float64Array. Mutates the underlying
 *     ArrayBuffer in place and returns the same array. Throws TypeError
 *     if the argument is not a Float64Array.
 *
 *   sortNumberArray(arr)
 *     Sort any JS Array of numbers. Copies into a Float64Array, sorts
 *     natively, copies back. ~O(n) extra work; usually still faster than
 *     pure JS for large n, but the zero-copy variant is preferred.
 *
 * Licensed under the LogosAdaptive Sort Ethical Source License v1.1.
 */

#define NAPI_VERSION 8
#include <node_api.h>
#include <assert.h>
#include <stdlib.h>
#include "sort.h"

/* ---------- helpers ---------- */

static void throw_type(napi_env env, const char *msg) {
    napi_throw_type_error(env, NULL, msg);
}
static void throw_err(napi_env env, const char *msg) {
    napi_throw_error(env, NULL, msg);
}

/* ---------- sortFloat64Array(arr) — zero-copy ---------- */

static napi_value sort_float64_array(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_status status;

    status = napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (status != napi_ok || argc < 1) { throw_type(env, "expected 1 argument"); return NULL; }

    bool is_ta = false;
    status = napi_is_typedarray(env, argv[0], &is_ta);
    if (status != napi_ok || !is_ta) { throw_type(env, "expected Float64Array"); return NULL; }

    napi_typedarray_type type;
    size_t length;
    void *data;
    status = napi_get_typedarray_info(env, argv[0], &type, &length, &data, NULL, NULL);
    if (status != napi_ok) { throw_err(env, "could not read typed array"); return NULL; }
    if (type != napi_float64_array) { throw_type(env, "expected Float64Array (got different TypedArray)"); return NULL; }

    int rc = ls_sort_double((double *)data, length);
    if (rc != 0) { throw_err(env, "logos-sort: allocation failure during sort"); return NULL; }

    return argv[0];
}

/* ---------- sortNumberArray(arr) — copies in, sorts, copies back ---------- */

static napi_value sort_number_array(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_status status;

    status = napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (status != napi_ok || argc < 1) { throw_type(env, "expected 1 argument"); return NULL; }

    bool is_array = false;
    status = napi_is_array(env, argv[0], &is_array);
    if (status != napi_ok || !is_array) { throw_type(env, "expected Array"); return NULL; }

    uint32_t len_u32;
    status = napi_get_array_length(env, argv[0], &len_u32);
    if (status != napi_ok) { throw_err(env, "could not get array length"); return NULL; }
    size_t n = (size_t)len_u32;

    if (n < 2) return argv[0];

    double *buf = (double *)malloc(n * sizeof(double));
    if (!buf) { throw_err(env, "logos-sort: allocation failure"); return NULL; }

    /* Copy in: read each element as a double */
    for (size_t i = 0; i < n; i++) {
        napi_value el;
        status = napi_get_element(env, argv[0], (uint32_t)i, &el);
        if (status != napi_ok) { free(buf); throw_err(env, "could not read element"); return NULL; }
        double v;
        status = napi_get_value_double(env, el, &v);
        if (status != napi_ok) { free(buf); throw_type(env, "all elements must be numbers"); return NULL; }
        buf[i] = v;
    }

    int rc = ls_sort_double(buf, n);
    if (rc != 0) { free(buf); throw_err(env, "logos-sort: allocation failure during sort"); return NULL; }

    /* Copy back */
    for (size_t i = 0; i < n; i++) {
        napi_value el;
        status = napi_create_double(env, buf[i], &el);
        if (status != napi_ok) { free(buf); throw_err(env, "could not create number"); return NULL; }
        status = napi_set_element(env, argv[0], (uint32_t)i, el);
        if (status != napi_ok) { free(buf); throw_err(env, "could not write element"); return NULL; }
    }

    free(buf);
    return argv[0];
}

/* ---------- version() ---------- */

static napi_value get_version(napi_env env, napi_callback_info info) {
    (void)info;
    napi_value out;
    napi_create_string_utf8(env, ls_sort_version(), NAPI_AUTO_LENGTH, &out);
    return out;
}

/* ---------- module init ---------- */

static napi_value init(napi_env env, napi_value exports) {
    napi_property_descriptor props[] = {
        { "sortFloat64Array", NULL, sort_float64_array, NULL, NULL, NULL, napi_enumerable, NULL },
        { "sortNumberArray",  NULL, sort_number_array,  NULL, NULL, NULL, napi_enumerable, NULL },
        { "version",          NULL, get_version,        NULL, NULL, NULL, napi_enumerable, NULL },
    };
    napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, init)
