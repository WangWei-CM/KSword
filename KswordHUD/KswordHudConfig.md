# KswordHUD Config

`KswordHUD` reads its runtime config from:

`Style/KswordHudConfig.json`

The `Style` directory is resolved relative to `KswordHUD.exe`.
If the file does not exist, the program creates it automatically on startup.

## Example

```json
{
  "backgroundImagePath": "HUD_background.png",
  "leftWidgetOpacityPercent": 100,
  "rightWidgetOpacityPercent": 100,
  "leftWidgetBackgroundColor": "#101820",
  "leftWidgetBackgroundOpacityPercent": 35,
  "leftProcessTableFontColor": "#FFFFFF",
  "rightWidgetBackgroundColor": "#08131F",
  "rightWidgetBackgroundOpacityPercent": 48
}
```

## Fields

`backgroundImagePath`

- Type: string
- Meaning: background image path
- Relative paths are resolved from the `Style` directory
- Absolute paths are also allowed

`leftWidgetOpacityPercent`

- Type: integer
- Range: `0` to `100`
- Meaning: total opacity of the whole left widget
- This affects the widget and all of its children together

`rightWidgetOpacityPercent`

- Type: integer
- Range: `0` to `100`
- Meaning: total opacity of the whole right widget
- This affects the widget and all of its children together

`leftWidgetBackgroundColor`

- Type: string
- Recommended format: `#RRGGBB`
- Meaning: background color of the left widget container

`leftWidgetBackgroundOpacityPercent`

- Type: integer
- Range: `0` to `100`
- Meaning: opacity of the left widget background color only
- This does not change child widget opacity by itself

`leftProcessTableFontColor`

- Type: string
- Recommended format: `#RRGGBB`
- Meaning: font color of the left process table
- Default: white

`rightWidgetBackgroundColor`

- Type: string
- Recommended format: `#RRGGBB`
- Meaning: background color of the right widget container

`rightWidgetBackgroundOpacityPercent`

- Type: integer
- Range: `0` to `100`
- Meaning: opacity of the right widget background color only
- This does not change child widget opacity by itself

## Notes

- `leftWidgetOpacityPercent` / `rightWidgetOpacityPercent` control the full widget tree.
- `leftWidgetBackgroundOpacityPercent` / `rightWidgetBackgroundOpacityPercent` control only the container background fill.
- If a color string is invalid, `KswordHUD` falls back to black for that container color.
- `0` background opacity means fully transparent background.
- If the background image path is invalid, `KswordHUD` falls back to its built-in dark placeholder background.
