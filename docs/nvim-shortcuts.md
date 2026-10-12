# Neovim shortcuts

Most shortcuts work in NORMAL mode. Press Esc to get there.
“Space → b” means press Space, release it, then press b.
Commands beginning with : need Enter afterward.

## Essentials

| Key / command | Action |
|---|---|
| i | Start typing before the cursor |
| a | Start typing after the cursor |
| o | Create a line below and start typing |
| Esc | Return to normal mode |
| Space → w | Save |
| :w | Save |
| :q | Quit / close the current window |
| :wq | Save and quit |
| :q! | Quit without saving unsaved changes |

## Undo and redo

| Key | Action |
|---|---|
| u | Undo the last change |
| Ctrl+r | Redo an undone change |

Press u repeatedly to undo several changes.
With persistent undo configured, saved undo history survives restarts.

Undo does not recover files deleted through the explorer or terminal.

## Moving around

| Key | Action |
|---|---|
| h / j / k / l | Left / down / up / right |
| w | Next word |
| b | Previous word |
| 0 | Beginning of the line |
| ^ | First nonblank character on the line |
| $ | End of the line |
| gg | Beginning of the file |
| G | End of the file |
| Ctrl+o | Jump back to a previous location |
| Ctrl+i | Jump forward again |

## Select, copy, paste, and delete

| Key | Action |
|---|---|
| v | Start selecting text |
| V | Select whole lines |
| y | Copy selected text |
| yy | Copy the current line |
| p | Paste after the cursor / below the line |
| P | Paste before the cursor / above the line |
| d | Delete selected text |
| dd | Delete the current line |
| x | Delete the character under the cursor |
| >> | Indent the current line |
| << | Unindent the current line |

With the clipboard configuration, copying and pasting use
the system clipboard. Deleting text can also replace its contents.

## Search within a file

| Key / command | Action |
|---|---|
| /text | Search forward for text |
| ?text | Search backward for text |
| n | Next match in the search direction |
| N | Previous match |
| Space → / | Clear search highlighting |

## File explorer

| Key / command | Action |
|---|---|
| Space → e | Open the built-in file explorer |
| :Vexplore | Open the explorer in a vertical split |
| :Rexplore | Return to the explorer after opening a file |

Inside the explorer:

| Key | Action |
|---|---|
| j / k | Move down / up |
| Enter | Open a file or directory |
| - | Go to the parent directory |
| % | Create a file |
| d | Create a directory |

## Find and open files

| Command | Action |
|---|---|
| :edit filename.c | Open a file |
| :find filename.c | Find a file under the working directory |
| :pwd | Show the working directory |
| :cd ~/code/project | Change the working directory |

Press Tab while entering a filename to complete it.

Search project C files for text:

:vimgrep /text/j **/*.c
:copen

Press Enter on a result to open it.

## C / C++ assistance

These require clangd to be running for the file.

| Key | Action |
|---|---|
| K | Show information about the symbol under the cursor |
| gd | Go to definition |
| Ctrl+o | Return after jumping to a definition |
| Space → d | Show the diagnostic at the cursor |
| Space → f | Format the current file |

Completion controls — in INSERT mode:

| Key | Action |
|---|---|
| Ctrl+j | Request completion |
| Ctrl+n | Select the next suggestion |
| Ctrl+p | Select the previous suggestion |
| Ctrl+y | Accept the selected suggestion |
| Ctrl+e | Dismiss suggestions |

## Compile

| Key | Action |
|---|---|
| Space → b | Save and compile the current C / C++ file |

Uses GCC for C and G++ for C++.
Compiling main.c creates an executable named main beside it.
This shortcut builds one source file, not a multi-file project.

Compiler errors and search results use the quickfix list:

| Command | Action |
|---|---|
| :copen | Open the results list |
| :cclose | Close it |
| :cnext | Jump to the next result |
| :cprev | Jump to the previous result |

Press Enter on an entry to jump to its location.

## Run inside Neovim

When the working directory contains your compiled program:

:botright split | terminal ./main

Replace main with your executable's name.

| Key / command | Action |
|---|---|
| i | Enter terminal input mode |
| Ctrl+\ then Ctrl+n | Leave terminal input mode |
| :q | Close the terminal window after the program finishes |

## Splits and windows

| Key / command | Action |
|---|---|
| :vsplit filename.c | Open a file beside the current one |
| :split filename.c | Open a file above/below the current one |
| Ctrl+w then w | Switch to the next window |
| Ctrl+w then h/j/k/l | Switch left/down/up/right |
| :q | Close the current window |

## Help and troubleshooting

| Command | Action |
|---|---|
| :help | Open Neovim's help |
| :help undo | Read about undo |
| :checkhealth vim.lsp | Check language-server support |
| :checkhealth vim.provider | Check clipboard/provider support |
