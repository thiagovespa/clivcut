# Contributing

## Commit messages

clivcut follows [Conventional Commits](https://www.conventionalcommits.org/). Messages are written in English.

```
<type>(<scope>): <summary>

<body: what changed and why, wrapped at 72 columns>
```

- **Summary:** imperative mood, lowercase, no trailing period, at most 72 characters ("add", not "added" or "adds").
- **Types:** `feat`, `fix`, `perf`, `refactor`, `test`, `docs`, `build`, `ci`, `chore`.
- **Scopes** (optional): a module (`core`, `media`, `render`, `engine`, `app`, `cli`) or an area (`audio`, `timeline`, `captions`, `export`, `packaging`, `roadmap`).
- **Body** (optional): explain the reason for the change and anything non-obvious. A short bullet list is fine for larger changes.
- **Breaking changes:** add `!` after the type/scope and a `BREAKING CHANGE:` footer, for example when the `.clivcut` format changes incompatibly.

Examples:

```
feat(audio): add loudness normalization and noise reduction
fix(media): pass FFmpeg numeric options as integers
docs(roadmap): add mobile versions (Android and iOS)
build(packaging): bundle ONNX Runtime in the AppImage
```

Keep each commit focused on one logical change, and make sure it builds and `ctest` passes.

## Translations

User-facing strings are written in English in the code (`qsTr()` in QML, `tr()` in `QObject` classes, `QCoreApplication::translate()` elsewhere and `QT_TRANSLATE_NOOP()` in catalogs). Translations live in `i18n/`:

- `clivcut_pt_BR.ts`, `clivcut_es.ts`: one file per language.
- `clivcut_en.ts`: plural forms only (`%n clip(s)` → "1 clip", "2 clips").

After adding or changing strings, refresh the files and fill in the new entries with Qt Linguist or any editor:

```sh
cmake --build build --target update_translations
linguist i18n/clivcut_pt_BR.ts
```

`tools/translate.py` does both steps from a table on stdin (`English|Português|Español`, or five fields for `%n` plurals) and reuses existing translations of the same text in other contexts:

```sh
python3 tools/translate.py <<'EOF'
Export|Exportar|Exportar
%n clip(s) copied|%n clipe copiado|%n clipes copiados|%n clip copiado|%n clips copiados
EOF
```

The `.qm` files are compiled and embedded in the app on every build. To add a language, add `i18n/clivcut_<code>.ts` to `TS_FILES` in `src/app/CMakeLists.txt` and its native name to `kLanguages` in `src/app/Language.cpp`.
