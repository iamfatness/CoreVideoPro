# Built-in graphics packs: font provenance

Each folder here is a graphics style pack (`docs/reference/graphics-packs.md`). CMake copies this
directory to `packs/` beside every build's executables; `scripts/package-alpha.ps1` stages it beside
`corevideo-native.exe` in the Windows install, and `scripts/make-macos-shell-bundle.sh` into
`CoreVideo Pro.app/Contents/Resources/packs/`. Every font is SIL Open Font License 1.1 and ships
unmodified, with its licence text as `fonts/OFL.txt` in its pack.

`GraphicsPack.ShippedPackFontsMatchTheirPinnedChecksums` fails if a file changes.


All files below were fetched on 2026-11-28 and checked byte for byte against their upstream commit:
each google/fonts, Cormorant and Montserrat file's git blob hash equals the blob at the named
commit. The Inter faces are from the official v4.1 release archive (tag commit named), unmodified.
A pack with two families carries both licence texts, verbatim, one after the other, in its `OFL.txt`.

## news-signal

- Source: https://github.com/google/fonts at `5026800714d07231937d92eb4cceabf06e05d74e`
- Licence: SIL Open Font License 1.1; copyright notices in `news-signal/fonts/OFL.txt`

| file | SHA-256 | from |
|---|---|---|
| news-signal/fonts/BarlowSemiCondensed-Regular.ttf | e8c7242f612b13d026e18a94b63b0fd079e91a3faf009d1b6981dbb5f16d1153 | https://raw.githubusercontent.com/google/fonts/5026800714d07231937d92eb4cceabf06e05d74e/ofl/barlowsemicondensed/BarlowSemiCondensed-Regular.ttf |
| news-signal/fonts/BarlowSemiCondensed-SemiBold.ttf | bd299f4bc5b44d30be8d42e9bad3a5df7d66af1cd55d0ed72a8b8916360a1424 | https://raw.githubusercontent.com/google/fonts/5026800714d07231937d92eb4cceabf06e05d74e/ofl/barlowsemicondensed/BarlowSemiCondensed-SemiBold.ttf |
| news-signal/fonts/BarlowSemiCondensed-Bold.ttf | 5bd6757e459a110da81b44532f0f43058e2dab2401688abca84f2fdbde193cbb | https://raw.githubusercontent.com/google/fonts/5026800714d07231937d92eb4cceabf06e05d74e/ofl/barlowsemicondensed/BarlowSemiCondensed-Bold.ttf |
| news-signal/fonts/OFL.txt | 186d750eb496a4c17a76385f82be6aea2ac1cf2de074a811d63786cf374ea73f | licence text, verbatim |

## sports-kinetic

- Source: https://github.com/google/fonts at `5026800714d07231937d92eb4cceabf06e05d74e`
- Licence: SIL Open Font License 1.1; copyright notices in `sports-kinetic/fonts/OFL.txt`

| file | SHA-256 | from |
|---|---|---|
| sports-kinetic/fonts/BarlowCondensed-Regular.ttf | 583cec5da3b84bc4dc7c9c72e2a565c94d34e431518b19d7e250b7830ad5f996 | https://raw.githubusercontent.com/google/fonts/5026800714d07231937d92eb4cceabf06e05d74e/ofl/barlowcondensed/BarlowCondensed-Regular.ttf |
| sports-kinetic/fonts/BarlowCondensed-Bold.ttf | e476562ec9c1e16cf16475895b511f08c804f438cc9a9f80a44ea50a0eeb5b65 | https://raw.githubusercontent.com/google/fonts/5026800714d07231937d92eb4cceabf06e05d74e/ofl/barlowcondensed/BarlowCondensed-Bold.ttf |
| sports-kinetic/fonts/BarlowCondensed-SemiBoldItalic.ttf | dc83cbbb7545a117ab9e4c10ed07d35eef6f563b77621a1d357f25a3194987d1 | https://raw.githubusercontent.com/google/fonts/5026800714d07231937d92eb4cceabf06e05d74e/ofl/barlowcondensed/BarlowCondensed-SemiBoldItalic.ttf |
| sports-kinetic/fonts/BarlowCondensed-BoldItalic.ttf | d8694f65cd70c3e5e600ead37657aab0adb5f63b275d0c1f6f4a7046de222145 | https://raw.githubusercontent.com/google/fonts/5026800714d07231937d92eb4cceabf06e05d74e/ofl/barlowcondensed/BarlowCondensed-BoldItalic.ttf |
| sports-kinetic/fonts/OFL.txt | 186d750eb496a4c17a76385f82be6aea2ac1cf2de074a811d63786cf374ea73f | licence text, verbatim |

## worship-grace

- Source: https://github.com/CatharsisFonts/Cormorant at `016784836d2ad47bc4c62b7f504a0a5875d85aeb`
- Source: https://github.com/JulietaUla/Montserrat at `555facfb2a18c72c3c0380f0d9c0f060453a9058`
- Licence: SIL Open Font License 1.1; copyright notices in `worship-grace/fonts/OFL.txt`

| file | SHA-256 | from |
|---|---|---|
| worship-grace/fonts/CormorantGaramond-Regular.ttf | aba4d0a0205a57abc489db3c1c1531b4f4e59fbe5c596912be5a3b821a06efc0 | https://raw.githubusercontent.com/CatharsisFonts/Cormorant/016784836d2ad47bc4c62b7f504a0a5875d85aeb/fonts/ttf/CormorantGaramond-Regular.ttf |
| worship-grace/fonts/CormorantGaramond-Italic.ttf | 006291ded05a9e8a58436474d9bb74e7477f494a5dfc11435ae2a2d9d74d4ed9 | https://raw.githubusercontent.com/CatharsisFonts/Cormorant/016784836d2ad47bc4c62b7f504a0a5875d85aeb/fonts/ttf/CormorantGaramond-Italic.ttf |
| worship-grace/fonts/CormorantGaramond-SemiBold.ttf | de1b7923fbc20ce742f6498d085ad0b64dc3625efbd8bdb2f01dc70e1e7a6edd | https://raw.githubusercontent.com/CatharsisFonts/Cormorant/016784836d2ad47bc4c62b7f504a0a5875d85aeb/fonts/ttf/CormorantGaramond-SemiBold.ttf |
| worship-grace/fonts/Montserrat-Regular.ttf | 3e8abe50c44c82e2242e97d1ec8c0d385c4890cdc50447bcdb8605c81a38cfb2 | https://raw.githubusercontent.com/JulietaUla/Montserrat/555facfb2a18c72c3c0380f0d9c0f060453a9058/fonts/ttf/Montserrat-Regular.ttf |
| worship-grace/fonts/Montserrat-Medium.ttf | dae47428bb041f9716604e0e07b5b0c8585b3bdd8183362f75c69fe7bb3cfaf4 | https://raw.githubusercontent.com/JulietaUla/Montserrat/555facfb2a18c72c3c0380f0d9c0f060453a9058/fonts/ttf/Montserrat-Medium.ttf |
| worship-grace/fonts/OFL.txt | a966906e1c04f674675833efbb46ce22021cb59d534c337469dc1b970d5e601e | licence text, verbatim |

## corporate-clarity

- Source: https://github.com/JulietaUla/Montserrat at `555facfb2a18c72c3c0380f0d9c0f060453a9058`
- Licence: SIL Open Font License 1.1; copyright notices in `corporate-clarity/fonts/OFL.txt`

| file | SHA-256 | from |
|---|---|---|
| corporate-clarity/fonts/Montserrat-Regular.ttf | 3e8abe50c44c82e2242e97d1ec8c0d385c4890cdc50447bcdb8605c81a38cfb2 | https://raw.githubusercontent.com/JulietaUla/Montserrat/555facfb2a18c72c3c0380f0d9c0f060453a9058/fonts/ttf/Montserrat-Regular.ttf |
| corporate-clarity/fonts/Montserrat-Medium.ttf | dae47428bb041f9716604e0e07b5b0c8585b3bdd8183362f75c69fe7bb3cfaf4 | https://raw.githubusercontent.com/JulietaUla/Montserrat/555facfb2a18c72c3c0380f0d9c0f060453a9058/fonts/ttf/Montserrat-Medium.ttf |
| corporate-clarity/fonts/Montserrat-SemiBold.ttf | b4e1563393d73fdff491a869441245aef31add2ec03d9c97a6dae4de07c52fd0 | https://raw.githubusercontent.com/JulietaUla/Montserrat/555facfb2a18c72c3c0380f0d9c0f060453a9058/fonts/ttf/Montserrat-SemiBold.ttf |
| corporate-clarity/fonts/OFL.txt | 8b7141c03fa4f8d44e6345d5d4931709290f0f67875e452e95ac1fd3a027802e | licence text, verbatim |

## esports-neon

- Source: https://github.com/google/fonts at `5026800714d07231937d92eb4cceabf06e05d74e`
- Licence: SIL Open Font License 1.1; copyright notices in `esports-neon/fonts/OFL.txt`

| file | SHA-256 | from |
|---|---|---|
| esports-neon/fonts/ChakraPetch-Regular.ttf | 98fcd638baa5c81ff0316b7538ce330ee3b23b1302726de3526d5933a8ecf986 | https://raw.githubusercontent.com/google/fonts/5026800714d07231937d92eb4cceabf06e05d74e/ofl/chakrapetch/ChakraPetch-Regular.ttf |
| esports-neon/fonts/ChakraPetch-SemiBold.ttf | 45264de3204ddbd5fb3e14a2402acd5c630d16650ae5fc221d2c52da46a6734b | https://raw.githubusercontent.com/google/fonts/5026800714d07231937d92eb4cceabf06e05d74e/ofl/chakrapetch/ChakraPetch-SemiBold.ttf |
| esports-neon/fonts/ChakraPetch-Bold.ttf | 65fbf76d95651697275e19db4d717c0e95a789ddd3476478b05292104db278a0 | https://raw.githubusercontent.com/google/fonts/5026800714d07231937d92eb4cceabf06e05d74e/ofl/chakrapetch/ChakraPetch-Bold.ttf |
| esports-neon/fonts/OFL.txt | 13831d02389d917d22fcfa6c79f98f8acbf61d230add0e73a4d4dc5f3bfb9e56 | licence text, verbatim |

## talk-studio

- Source: https://github.com/google/fonts at `5026800714d07231937d92eb4cceabf06e05d74e`
- Licence: SIL Open Font License 1.1; copyright notices in `talk-studio/fonts/OFL.txt`

| file | SHA-256 | from |
|---|---|---|
| talk-studio/fonts/Poppins-Regular.ttf | 7e65201e9b79159e2300267cc885e16c8dcef2424cdfa09a29bfb0980a94a7ba | https://raw.githubusercontent.com/google/fonts/5026800714d07231937d92eb4cceabf06e05d74e/ofl/poppins/Poppins-Regular.ttf |
| talk-studio/fonts/Poppins-Medium.ttf | 90373e7d838d32468438fc3e152dca0bdb12edcab99ea639f158790b1ba1fd05 | https://raw.githubusercontent.com/google/fonts/5026800714d07231937d92eb4cceabf06e05d74e/ofl/poppins/Poppins-Medium.ttf |
| talk-studio/fonts/Poppins-SemiBold.ttf | d3bf1bdaf0550e83da9ac0b1d1d9fe6db086835a83aa28578e609a394b9a0286 | https://raw.githubusercontent.com/google/fonts/5026800714d07231937d92eb4cceabf06e05d74e/ofl/poppins/Poppins-SemiBold.ttf |
| talk-studio/fonts/OFL.txt | 6be04893d770899a015649c7aa3b582f871b272f8747a92b78b17c3e5c8b2573 | licence text, verbatim |

## minimal-line

- Source: https://github.com/rsms/inter at `e3a3d4c57d5ecc01453a575621882a384c1995a3`
- Licence: SIL Open Font License 1.1; copyright notices in `minimal-line/fonts/OFL.txt`

| file | SHA-256 | from |
|---|---|---|
| minimal-line/fonts/Inter-Regular.ttf | 40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82 | https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip (extras/ttf/Inter-Regular.ttf) |
| minimal-line/fonts/Inter-Italic.ttf | bbc051dd204b5019a1aa0bc0ae2aa8a05ab13e7a3f979fa357631dc7feb6833a | https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip (extras/ttf/Inter-Italic.ttf) |
| minimal-line/fonts/Inter-Bold.ttf | 288316099b1e0a47a4716d159098005eef7c0066921f34e3200393dbdb01947f | https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip (extras/ttf/Inter-Bold.ttf) |
| minimal-line/fonts/OFL.txt | 262481e844521b326f5ecd053e59b98c8b2da78c8ee1bdbb6e8174305e54935a | licence text, verbatim |
