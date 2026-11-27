# Shadow Hearts: Covenant — Clean-Room Director's Cut English Translation Packs

Drop optional curated English translation JSON files in this directory to override automatic
translations with hand-polished English dialogue.

## Format (Zero Copyrighted Game Data)

Each `.json` file maps the **16-hex MD5 digest of the source Shift-JIS byte string** to your
original English text (automatically word-wrapped to 40 columns using `$` line breaks, while
preserving `%i32` / `%d` / `%s` engine control tokens):

```json
{
  "pack": "nahash_curated",
  "strings": {
    "0123456789abcdef": "Look! That must be the entrance to$the undersea temple of Nahash."
  }
}
```
