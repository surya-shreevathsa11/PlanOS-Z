"""Tiny, safe arithmetic evaluator for contract WCET / duration models.

Allowed: numbers, strings, names (skill args), + - * / // % unary -, comparisons,
`a if cond else b`, min(), max(). Nothing else - contracts are data, not code.
"""

from __future__ import annotations

import ast
import operator
from typing import Any, Mapping

_BIN = {
    ast.Add: operator.add,
    ast.Sub: operator.sub,
    ast.Mult: operator.mul,
    ast.Div: operator.truediv,
    ast.FloorDiv: operator.floordiv,
    ast.Mod: operator.mod,
}
_CMP = {
    ast.Eq: operator.eq,
    ast.NotEq: operator.ne,
    ast.Lt: operator.lt,
    ast.LtE: operator.le,
    ast.Gt: operator.gt,
    ast.GtE: operator.ge,
}
_FUNCS = {"min": min, "max": max}


class ExprError(ValueError):
    pass


def parse(src: str | int | float) -> ast.Expression:
    try:
        tree = ast.parse(str(src), mode="eval")
    except SyntaxError as exc:  # pragma: no cover - message formatting only
        raise ExprError(f"bad expression {src!r}: {exc.msg}") from exc
    _check(tree.body)
    return tree


def names_in(tree: ast.Expression) -> set[str]:
    return {n.id for n in ast.walk(tree) if isinstance(n, ast.Name) and n.id not in _FUNCS}


def _check(node: ast.AST) -> None:
    if isinstance(node, ast.Constant):
        if not isinstance(node.value, (int, float, str)) or isinstance(node.value, bool):
            raise ExprError(f"unsupported constant {node.value!r}")
    elif isinstance(node, ast.Name):
        pass
    elif isinstance(node, ast.BinOp) and type(node.op) in _BIN:
        _check(node.left)
        _check(node.right)
    elif isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
        _check(node.operand)
    elif isinstance(node, ast.Compare) and all(type(o) in _CMP for o in node.ops):
        _check(node.left)
        for c in node.comparators:
            _check(c)
    elif isinstance(node, ast.IfExp):
        _check(node.test)
        _check(node.body)
        _check(node.orelse)
    elif (
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id in _FUNCS
        and not node.keywords
    ):
        for a in node.args:
            _check(a)
    else:
        raise ExprError(f"disallowed syntax in expression: {type(node).__name__}")


def evaluate(tree: ast.Expression, env: Mapping[str, Any]) -> Any:
    return _eval(tree.body, env)


def _eval(node: ast.AST, env: Mapping[str, Any]) -> Any:
    if isinstance(node, ast.Constant):
        return node.value
    if isinstance(node, ast.Name):
        if node.id not in env:
            raise ExprError(f"unknown name {node.id!r}")
        return env[node.id]
    if isinstance(node, ast.BinOp):
        return _BIN[type(node.op)](_eval(node.left, env), _eval(node.right, env))
    if isinstance(node, ast.UnaryOp):
        return -_eval(node.operand, env)
    if isinstance(node, ast.Compare):
        left = _eval(node.left, env)
        for op, comp in zip(node.ops, node.comparators):
            right = _eval(comp, env)
            if not _CMP[type(op)](left, right):
                return False
            left = right
        return True
    if isinstance(node, ast.IfExp):
        return _eval(node.body if _eval(node.test, env) else node.orelse, env)
    if isinstance(node, ast.Call):
        return _FUNCS[node.func.id](*[_eval(a, env) for a in node.args])  # type: ignore[union-attr]
    raise ExprError("unreachable")  # pragma: no cover
