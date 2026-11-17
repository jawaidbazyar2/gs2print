# Epson MX-80 with Graphtrax

found roms at:

http://toastytech.com/files/ancientroms.html

the rom 2 file seems to be bitmap data.

The apple II video rom decoder doesn't work on it though you -can- see some character data. I wonder if its interleaved..

nope. But they are 9 bytes per character. The "matrix" is 9x9 in theory but each character font is 9x8, and there is likely an assumed empty column after each character due to spacing settings. And, they are on their sides.

It looks like they work hard to not draw the same dot twice in a row. Some characters have blank columns. It is unclear if they mean to repeat the last column, or just have a blank in it. Or do nothing.

Ah, right, each byte represents the vertical column of the print head (8 dots high). 

The bit values are reversed - 0 means a dot, 1 means no dot.

OK, draw each character and rotate , then output the character. Starting to look sensible now.
However, at character 0x1C, there is a dislocation. There are 4 columns of nonsense then the < character 
starts halfway through the matrix. Then succeeding characters are off by 4 columns.
Happens again at 0x38. It's the last half of the W, then 4 cols of nonsense, then the X starts, though it's shifted one column too far to the left.

There might be some data stuffed in every so often. If not, there may be another table somewhere that provides the offsets of the start of each character.

The 'extra' 4 bytes are FC,FD,FE, and FF. Perhaps they don't want to cross a 256 byte page boundary! There is legible stuff up through:
Character 0x83 (offset 0x04AB):
after that, it starts to get weird. Four chars with all pins on. 
Now, in previous iterations, I thought I saw stuff that looked like the TRS characters.
garbage-looking stuff starts at 0x0500 offset in the file.

"The character set consists of letters, various numeric and special characters[1] as well as 64 semigraphics called squots (square dots) from a 2×3 matrix.[3] These were located at code points 128 to 191 with bits 5-0 following their binary representation,[3] similar to alpha-mosaic characters in World System Teletext.[4][5] "

So each TRS-80 graphics character is represented by 6 bits. 64 semigraphics = 6 bits. in a 2 x 3 matrix. What we don't know is exactly how the epson maps these into an 

this discusses how the graphics characters are drawn:

https://mirrors.apple2.org.za/ftp.apple.asimov.net/documentation/hardware/printers/Epson%20MX-80%20Printer%20Manual.pdf

However it also suggests the text characters are 9x6. with 6th column always being blank.
The Graphtrax must have altered that?

these are 9x8. 

The print head is 9 pins. A line is 480 dots across. 

ok, this is from the MX80:
| mode | CPI | Dots per Char |
|------|-----|----------------|
| normal | 10 | 6 |
| condensed | 16.5 | 3.5 |
| expanded | 5 | 12 |
| condensed expanded | 8.25 | 7 |

The RX-80 has a different character set, and that's what I got the ROM from. They do both have a 9-pin head.

Maybe it would be better to start with an ImageWriter instead of this. It will be better documented. Let's shift to the ImageWriter II file..
