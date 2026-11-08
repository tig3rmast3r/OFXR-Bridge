# Global Advanced mode (V833, behavior updated by V914)

The authoritative setting is `[ofxr] AdvancedMode` in the main OFXR INI. It is
not stored in game profiles. Legacy per-game flags cannot override it; game
requests and resets preserve the global policy.

V833 originally changed visibility only. V914 replaces that behavior: Advanced
Off forces hidden settings to defaults, also in inactive generator profiles.
See [V914 Basic defaults](V914_BASIC_DEFAULTS.md) for the exact scope.

An already-bound game observes the global policy through shared memory. A policy
change normalizes its settings and publishes one revision through the existing
runtime apply/drain mechanism. Repeated observations do not churn revisions.
Snapshot polling adds no disk IO or blocking waits. The main INI remains the
only persisted Advanced flag, and no live INI file watcher is introduced.
Restart matching tray/game after manually changing the INI.

Profile loading and explicit saves normalize Basic settings. No bulk profile
rewrite is performed. Reset to defaults keeps the global Advanced preference;
visible Basic controls retain their normal configurable behavior.
