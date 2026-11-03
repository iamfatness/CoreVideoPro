# Graphics Designer Slice 5 data binding

This slice implements the Sources and names-list requirements in Phase 2 §4. The pinned design declares variables. Each playlist row determines where their values come from.

## Operator workflow

The Designer offers to create a Sources column for an extra variable. Sources shows the column and allows editing it for each slot. Operate lets a Guest row map an extra variable to a column while following Sources. The mapping remains on that row when saving a newer revision that still supports the variable. Removing a variable drops its mapping on the next pin. Stable column ids survive label edits. Values follow the assigned Sources slot, as name and title do today.

A names list supports additional CSV columns headed by variable ids. Import and edit round-trip the recognized columns and retain variant ids. Legacy one- and two-column lists continue to work. A missing cell falls back to the row value and then the template default. An explicitly empty text cell can clear a value. Column names and typed values are validated against the pinned variable definitions before sending them to native preparation.

Name and title retain their current behavior. Extra values are resolved at cue preparation with the selected guest or names-list entry. A missing mapping or invalid value falls back to the saved row value, then the template default. Existing documents load with empty mappings and custom values. Design samples stay private. No new native command or render path is required.

## Acceptance

Add a Location variable and column. Enter distinct locations for two assigned guests. Bind Location on a playlist row and Show each guest from Operate. Verify the corresponding text after saving and reopening the show. Import a names list with two locations, including a quoted comma, and Show both entries. Check fallback behavior and verify a legacy names list plays as before.
