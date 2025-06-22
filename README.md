# PaperPusher

**PaperPusher** is a fast, keyboard-driven PDF search and viewer for scientific papers.  
- Written in C, GTK, and Poppler.
- Vim-style and beginner-friendly keyboard shortcuts.
- Efficient metadata search for large academic collections.

## Features
- Fuzzy search by title, author, abstract, etc.
- Lightning-fast PDF viewing with keyboard navigation.
- Virtual scrolling and smart PDF caching.
- PaperParser: AI driven metadata recognition

## Build

You'll need GTK 3, Poppler-GLib, and GCC.  
```bash
cd src
gcc -o paperpusher *.c `pkg-config --cflags --libs gtk+-3.0 poppler-glib`

