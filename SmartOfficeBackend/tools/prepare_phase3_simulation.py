"""Create or safely reset the disposable Phase 3 simulation database."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


BACKEND_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(BACKEND_ROOT))

from app.database import DEFAULT_DATABASE_PATH, create_sqlite_engine  # noqa: E402
from app.seed import initialize_database  # noqa: E402


SIMULATION_DATABASE_NAME = "smart_office.phase3_simulation.db"
SIMULATION_DATABASE_PATH = BACKEND_ROOT / SIMULATION_DATABASE_NAME
PROTECTED_BACKUP_PREFIX = "smart_office.pre_final_integration."


def validate_simulation_target(database_path: Path) -> Path:
    """Allow only the exact disposable filename and reject protected databases."""
    resolved = database_path.expanduser().resolve()
    if resolved == DEFAULT_DATABASE_PATH.resolve():
        raise ValueError("Refusing to target the live smart_office.db database.")
    if resolved.name.startswith(PROTECTED_BACKUP_PREFIX):
        raise ValueError("Refusing to target a protected Phase 0 backup database.")
    if resolved.name != SIMULATION_DATABASE_NAME:
        raise ValueError(
            f"Simulation database filename must be exactly {SIMULATION_DATABASE_NAME}."
        )
    return resolved


def prepare_simulation_database(
    database_path: Path = SIMULATION_DATABASE_PATH,
    *,
    reset: bool = False,
) -> str:
    """Create/validate the simulation DB, optionally deleting only that exact file."""
    target = validate_simulation_target(database_path)
    if reset and target.exists():
        target.unlink()

    target.parent.mkdir(parents=True, exist_ok=True)
    target_engine = create_sqlite_engine(target)
    try:
        return initialize_database(target, target_engine)
    finally:
        target_engine.dispose()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Create or safely reset the disposable Phase 3 Smart Office database."
    )
    parser.add_argument(
        "--reset",
        action="store_true",
        help="Delete and recreate only smart_office.phase3_simulation.db.",
    )
    args = parser.parse_args()

    try:
        result = prepare_simulation_database(reset=args.reset)
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    action = "reset" if args.reset else "ready"
    print(f"Phase 3 simulation database {action}: {SIMULATION_DATABASE_PATH}")
    print(f"Initialization result: {result}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
