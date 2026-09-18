# 📚 Reddit Post Title:
# Introducing Emebala: Translating Knowledge Across Civilizations (Now Split into Reader & Chat)

---

# 📚 Introducing Emebala — Translating Knowledge Across Civilizations

"Eme-bala" is an ancient Mesopotamian term that signifies humanity's earliest translators and interpreters. They were the people responsible for transmitting knowledge and bridging different worlds.

History rarely remembers the names of individual translators, but the modern world exists because of them. If Mesopotamian mathematics had never reached Greece, Pythagoras might never have emerged. What would our world look like if the trade concepts of Assyria or the statecraft of Persia had never crossed borders?

On a deeply personal note: if I hadn’t been able to read the poems of Goethe and Heinrich Heine through someone else’s dedicated translation, I would never have met my wife. Knowledge transmission isn't just an academic topic to me — it literally shaped my destiny.

Years ago, during my military service in South Korea, I read over 500 books. Books gave me the strength to endure personal grief, startup failures, and my late mother's illness. Later, I met a brilliant young founder from Nigeria through a startup community. I desperately wanted to recommend a book by a South Korean entrepreneur that had deeply inspired me, but because no English translation existed, I couldn’t share it with him. Similarly, when reading Kautilya’s 4th-century BCE *Arthashastra*, I wanted to dive deeper into related historical texts, only to be stopped cold by language barriers.

Every day, the world pours out new knowledge online, and AI seems to be everywhere. But I wanted AI to serve real people who love books and cross-border communication. That is why I started Emebala.

---

### Why Emebala is Now Split into Two Tools: Reader & Chat

When I first posted about Emebala, it was a single prototype centered on reading. 

However, as I used it daily, I realized that **deep contemplative reading** and **instant everyday conversation** require completely different rhythms and user experiences. Trying to force both into one app felt clunky. 

So I decided to separate Emebala into two dedicated, lightweight tools:

1. **Emebala Chat** — For real-time typing and talking without constant copy-pasting.
2. **Emebala Reader** — For deep, synchronized bilingual reading of books and historical texts.

---

## 💬 1. Emebala Chat: Type Naturally, Press Enter, and Replace

I realized how much friction we tolerate every day: opening a browser tab, pasting foreign text, copying the translation, and pasting it back into Discord or Slack. Even worse, many translation tools pollute your Windows Clipboard History (`Win` + `V`) with private messages.

I built **Emebala Chat** as an ultra-lightweight, native Windows tool (pure C++20 and Win32, no Electron bloat):

* **In-Place Translation**: Type naturally in your native language in any app (Discord, Slack, KakaoTalk, browsers, in-game chat). When you hit `Enter`, the text you just typed is erased and instantly replaced with the translation right at your cursor.
* **No Clipboard Pollution**: It specifically bypasses Windows Clipboard History (`Win` + `V`), preserving your private messages.
* **Dual Engines**: Runs locally on-device using a local model (Hy-MT2-1.8B via llama.cpp with CUDA GPU acceleration or CPU), or via a built-in cloud engine (Google Translate, no API key setup needed).
* **Direct2D Pill Badge**: A small, floating status badge that lets you click to pause or switch languages, plus drag-to-translate (double `Ctrl`+`C`) with native Windows speech (TTS).

Here is a short 20-second video showing how it actually works on Windows:

https://youtu.be/dhvRvJWc1L0

---

## 📖 2. Emebala Reader: The Open Book

While Chat is built for quick communication, **Emebala Reader** is built for deep study and contemplation:

* **Synchronized Dual-Page View**: Reads your original text on one side and the sentence-aligned translation on the other (German, Korean, English, Japanese, etc.).
* **Universal Formats & Scans**: Supports EPUB, PDF, MOBI, TXT, Markdown, and even scanned book pages via local on-device OCR.
* **100% Offline & Private**: Powered locally via CTranslate2. Your books, highlights, and reading notes stay strictly on your machine.
* **Cultural Reading Themes**: Designed with calm palettes inspired by *Mesopotamian Clay Tablets*, *Joseon White Porcelain*, and *Traditional Hanji Paper*.
* **Current Status**: v0.10.0 is available now, and v0.20.0 (handling complex academic multi-column layouts and footnotes) is in active development.

---

## 🌍 Preserving Languages & Regional Dialects

Beyond major languages, we want Emebala to protect linguistic heritage. Through the Hy-MT2 model, it supports **33 world languages (1,056 bidirectional pairs)** plus **5 regional dialects and indigenous tongues** often ignored by mainstream tech: Cantonese, Tibetan, Uyghur, Mongolian, and Kazakh.

---

## 🧱 The Modern 'Stone Tablet'

Papyrus decays. Paper rots. CDs degrade, and floppy disks have already vanished. 

Despite humanity existing for hundreds of thousands of years, the reason we possess recorded history at all is because ancient people etched their thoughts onto stone and fired clay. That seemingly primitive medium survived empires and millennia to speak to us today.

Our lives are short. When contemplating the eons before us and the generations yet to come, we are merely passing through like cosmic dust. But the descendants who will wonder about our era are waiting for us in the future.

Emebala aims to be a digital 'Stone Tablet' that preserves and shares human knowledge freely across boundaries.

---

## 🔗 Download & Links

If you want to try either tool or inspect the source code, everything is open:

* 🌐 **Homepage**: https://www.emebala.org/
* ⚡ **Emebala Chat (Windows Installer)**: [Direct Download (.exe)](https://pub-9e1fd15dbb4e489c8010bb5617dbe4fb.r2.dev/Emebalachat_Setup_0.10.0.exe) | [GitHub](https://github.com/teamsunplaza/Emebalachat)
* 📖 **Emebala Reader (Releases)**: [GitHub Releases](https://github.com/teamsunplaza/emebala/releases)
* 📑 **User Manuals**: [English Manual (PDF)](https://emebala.org/assets/emebala_manual_en.pdf) | [Korean Manual (PDF)](https://emebala.org/assets/emebala_manual_kr.pdf)
* 📚 **Technical Documentation**: https://emebala.org/docs.html
* 🎬 **20s Demo Video**: https://youtu.be/dhvRvJWc1L0

---

## 🤝 Join Us

Emebala is built by **Team Sunplaza**, an independent studio based in Seoul, South Korea. 

If this project resonates with you, or if you have suggestions, bug reports, or feature ideas, please feel free to reach out. You can write in your native language—every voice matters.

* **Reddit**: r/emebala
* **Email**: teamsunplaza@gmail.com
* **Support / Sponsor**: https://teamsunplaza.gumroad.com/l/emebala

Thank you for reading, and I hope Emebala helps your books and conversations travel a little further.
