from __future__ import annotations

from pathlib import Path
from typing import Any

import duckdb
from duckdb import DuckDBPyConnection


def _candidate_extension_paths() -> list[Path]:
    package_dir = Path(__file__).resolve().parent
    candidates = [
        package_dir / "vex.duckdb_extension",
        package_dir.parent / "vex.duckdb_extension",
    ]
    return candidates


def _load_vex_extension(connection: DuckDBPyConnection) -> None:
    for path in _candidate_extension_paths():
        if path.exists():
            escaped_path = path.as_posix().replace("'", "''")
            connection.execute(f"LOAD '{escaped_path}'")
            return

    # Wheels built by this repository link the extension into the bundled
    # DuckDB package, so a plain LOAD usually succeeds without a path.
    connection.execute("LOAD vex")


def connect(
    database: str = ":memory:",
    *,
    read_only: bool = False,
    config: dict[str, Any] | None = None,
    auto_load_vex: bool = True,
    **kwargs: Any,
) -> DuckDBPyConnection:
    """Open a DuckDB connection and load the VEX extension by default."""

    merged_config = {"allow_unsigned_extensions": "true"}
    if config:
        merged_config.update(config)

    connection = duckdb.connect(database=database, read_only=read_only, config=merged_config, **kwargs)
    if auto_load_vex:
        _load_vex_extension(connection)
    return connection


__all__ = ["DuckDBPyConnection", "connect", "duckdb"]
