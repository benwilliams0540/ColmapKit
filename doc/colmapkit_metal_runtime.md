# ColmapKit Metal Runtime Decision Memo

Date: 2026-06-30

## Summary

- Proven behavior for COLMAP-side sparse parity is CPU-based:
  - `colmapkit_sparse_reconstruct` is functionally aligned with CLI sparse reconstruction metrics on the synthetic fixture when `--use_metal_matching` is disabled.
- The macOS XCFramework shape has been proved for build form and importability.
- Runtime acceleration has not yet been proven in this integration slice.
  - Metal matcher binaries are compiled and exposed.
  - Runtime execution still falls back to deterministic CPU matching on the tested host.
  - SiftMetal extraction is currently out of the default path.

## Evidence from strict comparison run

Ran:
```bash
python3 scripts/python/colmapkit_compare.py --colmap-bin build-codex-metal/src/colmap/exe/colmap --colmapkit-bin build-codex-metal/src/colmap/colmapkit/colmapkit_sparse_reconstruct --run-dir /private/tmp/colmapkit-compare-metal-run --generate-synthetic-fixture --use-metal-matching --force
```

Observed:
- `comparison-report.json` was written to `/private/tmp/colmapkit-compare-metal-run/comparison-report.json`.
- Command result: failed by design due runtime fallback.
- Runtime warning captured:
  - `Requested Metal SIFT descriptor matching, but the Metal backend is unavailable or failed at runtime; falling back to deterministic CPU matching.`
- This is treated as valid evidence that Metal matching did not execute on the host in this environment.

Ran with fallback preservation:
```bash
python3 scripts/python/colmapkit_compare.py --colmap-bin build-codex-metal/src/colmap/exe/colmap --colmapkit-bin build-codex-metal/src/colmap/colmapkit/colmapkit_sparse_reconstruct --run-dir /private/tmp/colmapkit-compare-metal-run --generate-synthetic-fixture --use-metal-matching --allow-metal-fallback --force
```

Observed:
- `passed: true`
- The same metric values matched exactly (`registered_images=8`, `sparse_points=612`, `observations=2453`, `mean_reprojection_error=0.338067...`).
- The fallback warning remains present, so this confirms parity under controlled fallback parity mode.

## Decision for Splats embedded path

1. Keep Splats embedded default at CPU SIFT + CPU matching for now (proven baseline).
2. Treat Metal matching as first acceleration candidate, but only after end-to-end no-fallback execution is proven in the packaging/runtime environment.
3. Keep SiftMetal extraction explicitly off by default in the embedded path until framework resource loading and runtime correctness are validated in end-to-end Splats use.

