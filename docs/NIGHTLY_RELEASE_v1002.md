# OFXR 0.4.0 development build V1002

- DLSS-G uses its own native provider context and allocator instead of sharing the game's NGX initialization, addressing the MSFS 2024/ReShade crash on first evaluation.
- Private-provider failure bypasses generation without falling back into the game's context. Guide ownership,queue and fence validation remain in force.
- Includes V1000 UI,V999 NR and V1001 guide-owner updates.

Development continues. MSFS 2024 in-game validation remains pending.
