# ctag
An ncurses-based tagger for music files. Inspired by [stag](https://github.com/smabie/stag). Created because easytag sucks, and stag's source code looks to suck with all the goto statements too. It is written in the C programming language.

## Contributions

If you want to contribute, fork this repository and make a pull request.

- Requesting a feature? Make an issue. I will check it and might discuss. Implementation difficulty is a factor in this.
- Reporting a bug? Make an issue.
- Made a feature? Make a pull request. First feature is evaluated. Then implementation. If the former is good but latter bad it will be reimplemented.

## ToDo list

### Priority 1
- [X] Panels and windows structure
- [X] Directory and File browser
- [X] Edit single file tags
- [X] Marking and editing multiple files

### Priority 2
- [ ] Unicode support for input entries
- [ ] Figure out that damn bug where it will sometimes get Alzheimers and display the wrong metadata information
- [ ] An option to convert all music to use id3v2 tags
- [ ] An option to make all music drop all id3v1 tags
- [ ] PgUp, PgDown, Home and End key support

### Priority 3
- [ ] Allow vim style regexes on metadata?
- [ ] Add a Fuzzy Finder search?
- [ ] Config file header(inspired by suckless software)
- [ ] Man pages/keybinds listed

## License

MIT License. See the LICENSE file for more details.
