{
  "targets": [
    {
      "target_name": "zephyr_napi",
      "sources": [ "src/zephyr_napi.cpp" ],
      "include_dirs": [ "../../include" ],
      "defines": [ "NAPI_VERSION=8" ],
      "conditions": [
        [ "OS=='win'", {
          "libraries": [ "../../build/zephyrdb.lib" ],
          "msvs_settings": {
            "VCCLCompilerTool": {
              "AdditionalOptions": [ "/std:c++20", "/EHsc" ]
            }
          }
        }],
        [ "OS!='win'", {
          "libraries": [ "-L../../build", "-lzephyrdb", "-Wl,-rpath,$ORIGIN/../../../../build" ],
          "cflags_cc": [ "-std=c++20", "-fexceptions" ]
        }]
      ]
    }
  ]
}
