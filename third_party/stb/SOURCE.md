# Vendored headers

Retrieved 2026-09-21. License text is retained in the headers and LICENSE.txt.

- `stb_image.h`: https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
  SHA-256: `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`
- `stb_image_write.h`: https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h
  SHA-256: `cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05`

The hashes above identify the originally retrieved upstream headers.

Local patch, 2026-09-24: `stb_image_write.h` formats the HDR header with
`snprintf(buffer, sizeof(buffer), ...)` instead of `sprintf`, which is deprecated
by current Apple SDKs. It rejects formatting errors/truncation before passing
the length to the write callback. Existing successful output is unchanged;
project-wide warning checks remain enabled.
