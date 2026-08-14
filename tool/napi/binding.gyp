{
  "targets": [
    {
      "target_name": "sprocket_node",
      "sources": [
        "sprocket_node.c",
        "../../sqlite3.c"
      ],
      "include_dirs": [ "../.." ],
      "defines": [
        "NAPI_VERSION=8",
        "SQLITE_THREADSAFE=1",
        "SQLITE_ENABLE_MATH_FUNCTIONS"
      ],
      "msvs_settings": {
        "VCCLCompilerTool": { "Optimization": 2 }
      }
    }
  ]
}
