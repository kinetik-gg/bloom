#pragma once

#include <string_view>

inline constexpr std::string_view kNodeLayoutLegacyDocument = R"legacy({
  "schemaVersion": {
    "major": 1,
    "minor": 0
  },
  "project": {
    "id": "1",
    "name": "Untitled Project",
    "colorSettings": {
      "schemaVersion": {
        "major": 1,
        "minor": 0
      },
      "processColorSpaceId": "lin_rec709_scene",
      "ocioConfig": {
        "schemaVersion": {
          "major": 1,
          "minor": 0
        },
        "locator": {
          "kind": "builtin",
          "uri": "bloom://ocio/neutral-v1/config.ocio"
        },
        "expectedRevision": {
          "algorithm": "sha256",
          "digest": "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
        },
        "portability": "builtin",
        "contextVariables": []
      }
    },
    "compositions": [
      {
        "id": "1",
        "name": "Main Composition",
        "duration": {
          "numerator": "10",
          "denominator": "1"
        },
        "format": {
          "width": 1920,
          "height": 1080,
          "pixelAspect": {
            "numerator": "1",
            "denominator": "1"
          },
          "frameRate": {
            "numerator": "24",
            "denominator": "1"
          }
        },
        "parameters": [],
        "animationCurves": [],
        "graph": {
          "nodes": [
            {
              "id": "1",
              "typeId": "bloom.layer-stack",
              "schemaVersion": 1,
              "parameters": []
            },
            {
              "id": "2",
              "typeId": "bloom.composition-output",
              "schemaVersion": 1,
              "parameters": []
            }
          ],
          "edges": [
            {
              "id": "1",
              "source": {
                "nodeId": "1",
                "port": "image"
              },
              "destination": {
                "kind": "node-input",
                "nodeId": "2",
                "port": "image"
              }
            }
          ],
          "layerOutputs": [],
          "layerStack": {
            "nodeId": "1",
            "entries": []
          },
          "compositionOutput": {
            "nodeId": "2",
            "port": "image"
          }
        }
      }
    ]
  },
  "idAllocation": {
    "highestIssued": {
      "composition": "1",
      "node": "2",
      "edge": "1",
      "layer": "0",
      "layerSlot": "0",
      "parameter": "0",
      "animationCurve": "0",
      "keyframe": "0",
      "driverBinding": "0",
      "extensionRecord": "0"
    }
  },
  "extensions": []
}
)legacy";
