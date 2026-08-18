# Drafted Plan

Extensible AI agent harness built with C. As much as makes sense of the app would be implemented using extensions. Extensions could be written by the user and agents to extend the functionality of the app.
Core of the app should focus only to the absolute minimum that is needed to have solid base for an agent harness.

Should try to avoid complexity. Minimal dependencies and when dependencies are needed they should also try to be minimal. Safety is not a priority, for example there will be no native permission system or folder sandboxing.
If user wants more safety they should implement the features as extensions. 

## Main features
0. Extension system
  1. Most things below are implemented as an extension. If possible everything is implemented as extension
1. Markdown rendering chat interface
  1. Virtual scrolling would be a good idea
2. Editor
  1. Basic keybinds (ctrl+direction, ctrl+w, etc.)
  2. Paste of long content (Most likely temp file created at /tmp)
3. Agent loop
4. Workspaces
5. Compaction

## Core
Somekind of core module that handles:
1. Basic agent loop and tool loading
  2. Can be very simple see for inspiration https://raw.githubusercontent.com/smol-env/smol/refs/heads/main/smol.py
3. Plugin loader
4. Session management
5. LLM connection and auth handling
  1. Start with openai oauth and custom openai responses api support
  2. Model discovery
8. Model management
  1. Can be very simple main important things are: model name, id, context limits, capabilities (vision, etc.)
10. Settings management (persisted in a config)
11. Render function stub
  1. Extensions can actually provide the rendering code
    1. This requires that the extensions can read and modify pretty much full app state if needed
  2. There would be a default UI extension that renders a default chat interface
  3. Editor UI (mainly the text box) would need an extension
  4. Footer UI extension to show context usage and other such information
  5. There could be also a workspace changer UI sidebar extension.

## Reloading
So when user uses the agent to create a new extension or modifies existing one how do we reload the app? Do we have some kind of live reload, manual reload, app restart?

Most likely it would be good that the core app survives even when a extension fails and preferably can give some kind of a warning. At least a mode which loads the core with only the build in extension should be added.