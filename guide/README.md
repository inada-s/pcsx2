# Player setup guide

Source of the player setup guide on GitHub Pages (Jekyll), published by
`.github/workflows/zdxsv_guide.yml` on each push to `zdxsv-master` that changes `guide/`.

- One page per step group; each step is an `###` heading, one action and one screenshot.
- Screenshots go in `images/` and are shown with
  `{% include shot.html file="name.png" alt="what it shows" %}`; a missing file shows a placeholder box.
- Screenshots in Japanese UI, PNG, the window only, the button to press boxed in red.
- Text for a PC beginner: only the action each step needs, no side notes.
