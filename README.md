# Reacto

Phone-powered cardboard creations for kids. Build something out of cardboard, put a phone in it, and the phone brings it to life.

## Creations

### 👀 Eyes (`/eyes/`)
The whole interface is landscape. The first screen asks kids to turn the phone sideways and lock rotation.
If the phone is locked in portrait (iPhone's rotation lock only locks portrait), the page rotates itself and
detects which way the phone is turned; use **⇅ Flip** on the first screen if it comes out upside down.

1. Pick one of 6 pairs of eyes (Classic, Googly, Cat, Robot, Sparkly, Monster).
2. Pick a face/background color.
3. Press **Go!** — tilt the phone and the pupils roll toward the low side; shake it and the eyes get confused (eyes spin, eyebrows go up, question marks float).

Controls while playing:
- **Tap** – blink
- **Double-tap** – re-center ("look straight ahead" from the current position)
- **Hold ⚙** (top-left) – go back and change eyes
- On a computer: move the mouse to look around, press **Space** to get confused

Tip: hold it in the position it will sit in the creation when pressing Go (that becomes "looking straight").

## Hosting on GitHub Pages
Settings → Pages → Source: **Deploy from a branch** → Branch: `main`, folder `/ (root)` → Save.
The site will be at `https://hellonun.github.io/reacto/` and the eyes at `https://hellonun.github.io/reacto/eyes/`.

Motion sensors only work over HTTPS, which GitHub Pages provides. On iPhone the page will ask for permission to use motion when you press Go.
