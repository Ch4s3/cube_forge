# Probes

Small standalone forge projects / files that reproduce each finding in
`GAPS.md`. Each directory is a complete forge project: `cd` in and run
`forge build && ./.march/build/debug/<name>` (or `march --check <file>`).

- `fieldcall.march` — G3, field access on a call result does not parse
- `probe1/` — G8 (Bytes over FFI), G9 (NativeArray over FFI), G12 (record vs variant allocation)
- `externmod2/` — G6, qualified extern call with args → "Unknown module"
- `externmod3/` — G5 (qualified extern → link failure), G11 (caller needs no cap)
