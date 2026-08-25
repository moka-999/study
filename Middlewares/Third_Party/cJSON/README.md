# cJSON vendoring record

- Upstream: `https://github.com/DaveGamble/cJSON`
- Version: `v1.7.19`
- Files compiled: `cJSON.c`, `cJSON.h`
- License: MIT, preserved in `LICENSE`
- Local compile policy: `CJSON_NESTING_LIMIT=16`, locale support disabled

SHA-256:

- `cJSON.c`: `298581A04A36C0165DA4B0AADE235C23088CB2FAA58651D720EA2F3706ED0B0D`
- `cJSON.h`: `25B0145150D500498E4D209CEC69C18C42CF818BFFCC54690BE3B895A2A16DEE`
- `LICENSE`: `A36DDA207C36DB5818729C54E7AD4E8B0C6FBA847491BA64F372C1A2037B6D5C`

Only the two upstream source files are integrated. `cJSON_Utils` and the
upstream build system are intentionally not imported.
