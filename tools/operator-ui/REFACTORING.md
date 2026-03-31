# Operator UI Refactoring - February 2026

## Summary

Refactored the Operator UI from a device-centric tab interface to a unified single-page dashboard with modular JavaScript architecture.

## Changes

### UI/UX Improvements

**Before:**

- Device selection sidebar with tabs
- Per-device views that showed mostly duplicate automation panels
- Confusion about what was device-specific vs. runtime-global
- Blank screen on initial load

**After:**

- Single unified dashboard
- Clear separation: Automation Control (runtime-global) + Device Status Overview (all devices at once)
- No device selection needed - see everything at a glance
- Clean, modern card-based layout

### Code Architecture

**Before:**

- Single monolithic `app.js` (1253 lines)
- Mixed concerns (API, SSE, UI, logic all together)
- Difficult to maintain and extend

**After:**
Modular structure:

```sh
js/
├── config.js        - Constants and configuration
├── api.js           - HTTP API client functions
├── sse.js           - SSE connection management (event bus pattern)
├── ui.js            - Shared UI utilities (formatting, DOM helpers)
├── automation.js    - Automation UI module (mode, parameters, BT, events)
├── devices.js       - Device overview module (all device status cards)
└── app.js           - Main entry point (115 lines)
```

### Key Features

1. **Event Bus Pattern:** SSE module uses event emitters - other modules subscribe to events
2. **Separation of Concerns:** Each module has single responsibility
3. **ES6 Modules:** Proper `import`/`export` for clean dependencies
4. **Maintainable:** Easy to add new features or modify existing ones

## File Backups

Old files preserved as:

- `index-old.html` (original HTML)
- `app-old.js` (original monolithic script)
- `style-old.css` (original stylesheet)

## Testing

**To test the new UI:**

1. Start runtime from repo root:
   - `cmake --preset dev-windows-release`
   - `cmake --build --preset dev-windows-release --parallel`
   - `.\build\dev-windows-release\core\Release\anolis-runtime.exe --config .\anolis-runtime.yaml`
2. Open browser: <http://localhost:3000>
3. Verify:
   - Automation Control panel shows mode, parameters, BT
   - Device Status cards show all devices at once
   - Event trace logs mode/parameter changes
   - Real-time updates via SSE (no connection churn)
   - No device selection needed

## Migration Notes

- **No breaking API changes** - still uses same HTTP/SSE endpoints
- **Backward compatible** - can roll back by swapping old files
- **Browser compatibility** - Requires ES6 module support (modern browsers)

## Future Enhancements

Possible additions now that code is modular:

- Collapsible device cards with full signal list
- Interactive BT visualization (instead of text outline)
- Filtering/search in event trace
- Device function execution UI
- Dashboarding/charting for signal trends
