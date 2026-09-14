{
  "targets": [
    {
      "target_name": "logos_sort_native",
      "sources": [
        "binding/sort_napi.c",
        "c/sort.c"
      ],
      "include_dirs": [
        "c"
      ],
      "cflags": [
        "-O2",
        "-Wall",
        "-Wextra",
        "-std=c99",
        "-fvisibility=hidden"
      ],
      "cflags!": [ "-fno-exceptions" ],
      "cflags_cc!": [ "-fno-exceptions" ],
      "defines": [
        "NAPI_VERSION=8"
      ],
      "conditions": [
        ["OS==\"mac\"", {
          "xcode_settings": {
            "GCC_C_LANGUAGE_STANDARD": "c99",
            "OTHER_CFLAGS": [ "-O2", "-Wall", "-Wextra" ]
          }
        }],
        ["OS==\"win\"", {
          "msvs_settings": {
            "VCCLCompilerTool": {
              "AdditionalOptions": [ "/O2" ]
            }
          }
        }]
      ]
    }
  ]
}
