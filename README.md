# ctag
An ncurses-based tagger for music files. Inspired by [stag](https://github.com/smabie/stag). Created because I don't like using easytag, and stag's source code is rough to change. It is written in the C programming language.

## Features:

- TUI interface to change metadata tags of music files
- 2 Panel layout, one for setting metadata, one for browsing directory
- Uses id3v2 for tagging files
- Edit individual metadata for a file
- Selecting files in an order to set their track number
- Multifile metadata editing. Select multiple files then edit them as if editing a single file. Tags common across all are listed.
- Recursively setting the Artist name from a Directory name(with an input modal so you can change this if the directory name is incorrect)
- Recursively setting the Album name from a Directory name(with an input so you can change this if the directory name is incorrect)
- Recursively setting the Genres and Years of a Directory
- Fuzzy Finder, Find and navigate to directories by knowing part of the file or directory name
- Unicode UTF-8 support

## Contributions

If you want to contribute, fork this repository and make a pull request.

- Requesting a feature? Make an issue. I will check it and might discuss. Implementation difficulty is a factor in this.
- Reporting a bug? Make an issue.
- Made a feature? Make a pull request. First feature is evaluated. Then implementation. If the former is good but latter bad it will be reimplemented.

Note: This is a small side project that is also an experiment on my part to see how viable LLMs are currently for general purpose programming. I have a nuanced opinion on LLMs currently, see my blog post on the [subject](https://risingthumb.xyz/Writing/Blog/Current_Opinions_on_AI_Generation), and this project would likely remain in my graveyard of ideas and projects if I didn't poke around with LLMs for it. If you don't like AI, consider this your AI notice, and look at stag for an alternative.

## ToDo list

### Priority 1
- [X] Panels and windows structure
- [X] Directory and File browser
- [X] Edit single file tags
- [X] Marking and editing multiple files

### Priority 2
- [X] Unicode support for input entries
- [X] Figure out that damn bug where it will sometimes get Alzheimers and display the wrong metadata information
- [X] PgUp, PgDown, Home and End key support
- [X] An option to convert all music to use id3v2 tags
- [ ] An option to make all music drop all id3v1 tags
- [ ] An option to drop all unrelated tags(ogg and other formats have weird tags that can interact strangely in some music players)

### Priority 3
- [X] Add a Fuzzy Finder search?
- [ ] Allow vim style regexes on metadata?
- [ ] Config file header(inspired by suckless software)
- [ ] Man pages/keybinds listed

## License

MIT License. See the LICENSE file for more details.
