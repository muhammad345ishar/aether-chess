"""Deadline protection for synchronous python-chess engine searches."""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, TimeoutError as FutureTimeout

import chess
import chess.engine


def bounded_play(
    engine: chess.engine.SimpleEngine,
    board: chess.Board,
    limit: chess.engine.Limit,
    *,
    executor: ThreadPoolExecutor,
    timeout: float,
    **kwargs: object,
) -> chess.engine.PlayResult:
    """Play with a response deadline without changing the engine's search limit.

    python-chess does not impose its engine timeout on node-only searches.
    A caller-shared worker lets us bound those calls too. For timed searches,
    the response deadline includes both the requested thinking time and the
    timeout allowance. Closing a stalled engine releases its blocked worker
    before the caller shuts down the executor.
    """
    deadline = timeout + (limit.time or 0.0)
    future = executor.submit(engine.play, board, limit, **kwargs)
    try:
        return future.result(timeout=deadline)
    except FutureTimeout as exc:
        engine.close()
        future.cancel()
        raise TimeoutError(f"engine did not return a move within {deadline:g} seconds") from exc
