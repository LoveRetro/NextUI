NextUI Color Palettes
----------------------------------------

Drop custom color palette files (.txt) in this folder. They will show up under
Settings > Appearance > Color Palette, alongside the built-in palettes, and can be
shared with others by simply copying the file.

Each palette is a plain text file. Example (my_palette.txt):

    version=1
    name=Example
    color1=0xffffffff
    color2=0x9b2257ff
    color3=0x1e2329ff
    color4=0xffffffff
    color5=0x000000ff
    color6=0x000000ff
    color7=0xffffffff

Fields
------
version   Palette file format version. Use 1. Files with a version newer than the
          running NextUI build supports are ignored.
name      The label shown in the menu. If omitted, the file name is used (with
          underscores turned into spaces).
color1    Main color - main UI elements.
color2    Primary accent - highlights important things.
color3    Secondary accent.
color4    List text.
color5    List text (selected).
color6    Hint / info text (button hints).
color7    Background - used when no background image is set.

Colors are packed hex. Both 0xRRGGBB and 0xRRGGBBAA are accepted; when the alpha
byte is omitted the color is treated as fully opaque. Any color you leave out
falls back to the NextUI default for that slot.

Built-in palettes live with the system files and cannot be edited. Copy one here
and rename it to use it as a starting point for your own.
