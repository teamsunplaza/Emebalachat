"""UI package for Emebalachat.

Exports the floating status badge overlay and the system tray manager.
"""

from .badge import FloatingBadge, BadgeState
from .tray import TrayManager

__all__ = [
    "FloatingBadge",
    "BadgeState",
    "TrayManager",
]
