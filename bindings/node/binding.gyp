{
  "targets": [
    {
      "target_name": "urb_ffi",
      "sources": [
        "src/urb_ffi_napi.c",
        "../../src/urbc_format.c",
        "../../src/urbc_platform.c",
        "../../src/urbc_api.c",
        "../../src/urbc_ffi_sig.c",
        "../../src/urbc_loader.c",
        "../../src/urbc_schema.c",
        "../../src/urbc_runtime.c",
        "../../src/urbc_ops_core.c",
        "../../src/urbc_ops_mem.c",
        "../../src/urbc_ops_schema.c",
        "../../src/urbc_ops_ffi.c"
      ],
      "include_dirs": [
        "src",
        "../../include",
        "../../src",
        "<!@(node ./scripts/libffi-config.js include-dirs)"
      ],
      "defines": [
        "BUILDING_NODE_EXTENSION",
        "_CRT_SECURE_NO_WARNINGS"
      ],
      "cflags": [
        "<!@(node ./scripts/libffi-config.js cflags)"
      ],
      "conditions": [
        ["OS=='linux' or OS=='freebsd' or OS=='openbsd'", {
          "libraries": [
            "<!@(node ./scripts/libffi-config.js libraries)",
            "-ldl",
            "-lm"
          ]
        }],
        ["OS=='mac'", {
          "libraries": [
            "<!@(node ./scripts/libffi-config.js libraries)",
            "-lm"
          ]
        }],
        ["OS=='win'", {
          "libraries": [
            "<!@(node ./scripts/libffi-config.js libraries)"
          ]
        }]
      ]
    }
  ]
}
