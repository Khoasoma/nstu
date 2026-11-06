# Interim UI03: supplied NSTU brand asset

Owner: root, sole source writer and build owner. The user requested the exact Asset_54x.png image. Six-file allowlist: this plan; server/app/main.cpp (brand include/object, device initialization/teardown and sidebar mark only); server/app/brand_image.hpp; server/app/brand.rc.in; server/CMakeLists.txt (resource wiring only); exam/web/assets/nstu-brand.png (unchanged input bytes).

Base: 725ad855cbb71a92602f4751d2c016289c2c19ef. Source main SHA256 b826fe1f9ebb8003b6e334a5a30413bdf8954c176d9bff9593aa2b1154d630af; server CMake a031fd876178e8c03e6d9ff1c7325b701709035ad39e390f8c0e9dd038418099. Asset SHA256 0b3cf7023212288d978bd898d0fa9ec1dcb7e82c92b678fa2bd1d42e18272486, 2386x1037 RGBA. Independent PLAN_ACCEPT fdca76aceec71103a9aee5d33a02fd372b684c454d734bab1962a6a1ab034374.

Acceptance: preserve the supplied image bytes and transparency; embed the asset in the executable; decode/upload once with balanced COM and D3D11 ownership; render the original aspect within the DPI-scaled sidebar; retain a text fallback and all existing commands/roles/navigation. Later web surface slices reuse the canonical asset. No controller, protocol or pairing changes.

Verification: server build; independent WARP texture/resource byte and alpha probe; focused presentation/roster tests; fast and strict gates; independent nonauthor code review. The user stopped desktop inspection with physical Escape. No further desktop control is used for this slice; full native visual acceptance remains pending.
