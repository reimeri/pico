# Clay Markdown Reader

Welcome to the **Clay** markdown reader sample. This document exercises
every feature of the renderer: *headings*, **bold**, *italic*,
***bold italic***, `inline code`, [links](https://github.com/nicbarker/clay),
lists, quotes, images and code blocks.

Save this file while the app is running — it will reload automatically.

## Headings

### Third level heading

#### Fourth level heading

##### Fifth level heading

###### Sixth level heading

## Inline styles

This paragraph mixes styles within a single wrapping paragraph: some
**bold text**, some *italic text*, some ***bold italic*** and some
`inline_code_here()` sprinkled between normal words so you can see how
the runs wrap together across multiple lines when the window is narrow.
Links like [the clay repository](https://github.com/nicbarker/clay) and
[md4c](https://github.com/mity/md4c) open in your browser when clicked.

Autolinks are recognized too: https://clayui.com

A hard break uses two trailing spaces:  
first line  
second line

## Lists

Unordered list:

- First item
- Second item with **bold** and `code`
  - Nested item one
  - Nested item two
    - Even deeper
- Fourth item

Ordered list:

1. Step one
2. Step two
3. Step three with an [embedded link](https://example.org)

Task list:

- [x] Plan the app
- [x] Parse markdown with md4c
- [ ] Ship it

## Quotes

> Blockquotes get a left accent border and a subtle background.
> They can wrap over multiple lines and still support **bold**,
> *italic* and `code` inside them.

## Code

Fenced code block:

```c
#include <stdio.h>

int main(void)
{
    // Long lines are not wrapped inside code blocks
    printf("Hello from a very long line inside a fenced code block that definitely exceeds the width of the window and should simply overflow\n");
    return 0;
}
```

Indented code block:

    let x = 10
    let y = 20
    print(x + y)

## Images

A local image (relative to this file):

![The sample profile picture](resources/profile-picture.png)

A missing image shows a fallback message:

![this file does not exist](resources/missing.png)

Remote images are not downloaded:

![remote image](https://example.org/image.png)

## Horizontal rule

Before the rule.

---

After the rule.

## Entities and escaping

Named entities like &amp;, &lt;, &gt; and &quot; are decoded. Numerical
entities work too: &#65;&#66;&#67; and &#x2603;.

Escaped characters: \*not italic\*, \#not a heading, \`not code\`.

## A long final paragraph

Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod
tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim
veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea
commodo consequat. **Duis aute irure dolor** in reprehenderit in
voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur
sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt
mollit anim id est laborum. Sed ut perspiciatis unde omnis iste natus
error sit voluptatem accusantium doloremque laudantium, totam rem
aperiam, eaque ipsa quae ab illo inventore veritatis et quasi architecto
beatae vitae dicta sunt explicabo. Nemo enim ipsam voluptatem quia
voluptas sit aspernatur aut odit aut fugit, sed quia consequuntur magni
dolores eos qui ratione voluptatem sequi nesciunt.
