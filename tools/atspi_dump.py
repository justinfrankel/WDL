#!/usr/bin/env -S uv run --script
# /// script
# dependencies = ["dbus-next"]
# ///

from __future__ import annotations

import argparse
import asyncio
from dataclasses import dataclass
from typing import Iterable

from dbus_next.aio import MessageBus
from dbus_next.constants import BusType
from dbus_next.errors import DBusError, InterfaceNotFoundError


REGISTRY_SERVICE = "org.a11y.atspi.Registry"
ROOT_PATH = "/org/a11y/atspi/accessible/root"


@dataclass(frozen=True)
class NodeRef:
    service: str
    path: str


class NodeProxy:
    def __init__(self, bus: MessageBus, ref: NodeRef):
        self.bus = bus
        self.ref = ref
        self._accessible = None
        self._component = None

    async def _introspect(self):
        return await self.bus.introspect(self.ref.service, self.ref.path)

    async def accessible(self):
        if self._accessible is None:
            obj = self.bus.get_proxy_object(self.ref.service, self.ref.path, await self._introspect())
            self._accessible = obj.get_interface("org.a11y.atspi.Accessible")
            try:
                self._component = obj.get_interface("org.a11y.atspi.Component")
            except InterfaceNotFoundError:
                self._component = False
        return self._accessible

    async def component(self):
        await self.accessible()
        return self._component if self._component is not False else None

    async def describe(self) -> dict:
        acc = await self.accessible()
        try:
            role_name = await acc.call_get_role_name()
        except AttributeError:
            try:
                role_name = await acc.call_get_localized_role_name()
            except AttributeError:
                role_name = f"role-{await acc.call_get_role()}"
        result = {
            "name": await acc.get_name(),
            "role": role_name,
            "child_count": await acc.get_child_count(),
            "interfaces": await acc.call_get_interfaces(),
            "description": await acc.get_description(),
        }
        comp = await self.component()
        if comp is not None:
            try:
                x, y, w, h = await comp.call_get_extents(0)
                result["extents"] = (x, y, w, h)
            except DBusError:
                pass
        return result

    async def children(self) -> list[NodeRef]:
        acc = await self.accessible()
        return [NodeRef(service, path) for service, path in await acc.call_get_children()]


async def get_atspi_address() -> str:
    bus = await MessageBus(bus_type=BusType.SESSION).connect()
    intro = await bus.introspect("org.a11y.Bus", "/org/a11y/bus")
    obj = bus.get_proxy_object("org.a11y.Bus", "/org/a11y/bus", intro)
    iface = obj.get_interface("org.a11y.Bus")
    return await iface.call_get_address()


async def connect_atspi_bus() -> MessageBus:
    return await MessageBus(bus_address=await get_atspi_address()).connect()


def format_node_line(ref: NodeRef, info: dict, depth: int) -> str:
    indent = "  " * depth
    name = info["name"] or "<unnamed>"
    role = info["role"] or "<unknown>"
    extra = f"children={info['child_count']}"
    if "extents" in info:
        x, y, w, h = info["extents"]
        extra += f" rect=({x},{y} {w}x{h})"
    return f"{indent}- {name} [{role}] {ref.service} {ref.path} {extra}"


async def walk_tree(bus: MessageBus, roots: Iterable[NodeRef], max_depth: int, query: str | None) -> int:
    seen: set[NodeRef] = set()
    matches = 0

    async def walk(ref: NodeRef, depth: int):
        nonlocal matches
        if ref in seen:
            return
        seen.add(ref)

        try:
            node = NodeProxy(bus, ref)
            info = await node.describe()
        except DBusError as exc:
            print(f"{'  ' * depth}- <error> {ref.service} {ref.path}: {exc}")
            return

        haystack = " ".join(
            [info.get("name") or "", info.get("role") or "", ref.service, ref.path]
        ).lower()
        if query is None or query in haystack:
            print(format_node_line(ref, info, depth))
            matches += 1

        if depth >= max_depth:
            return

        for child in await node.children():
            await walk(child, depth + 1)

    for root in roots:
        await walk(root, 0)

    return matches


async def list_apps(bus: MessageBus):
    root = NodeProxy(bus, NodeRef(REGISTRY_SERVICE, ROOT_PATH))
    count = 0
    for child in await root.children():
        info = await NodeProxy(bus, child).describe()
        print(format_node_line(child, info, 0))
        count += 1
    if count == 0:
        print("<no AT-SPI applications found>")


async def main_async(args: argparse.Namespace):
    bus = await connect_atspi_bus()
    roots = [NodeRef(REGISTRY_SERVICE, ROOT_PATH)]

    if args.apps:
        await list_apps(bus)
        return

    if args.service or args.path:
        if not (args.service and args.path):
            raise SystemExit("--service and --path must be provided together")
        roots = [NodeRef(args.service, args.path)]

    matches = await walk_tree(bus, roots, args.max_depth, args.find.lower() if args.find else None)
    if matches == 0:
        print("<no matching accessibles found>")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Dump the Linux AT-SPI accessibility tree.")
    parser.add_argument("--apps", action="store_true", help="List top-level applications on the AT-SPI bus.")
    parser.add_argument("--find", metavar="TEXT", help="Only print nodes whose name/role/service/path contains TEXT.")
    parser.add_argument("--max-depth", type=int, default=3, help="Traversal depth for tree dumps. Default: 3.")
    parser.add_argument("--service", help="AT-SPI service name for a direct subtree dump.")
    parser.add_argument("--path", help="AT-SPI object path for a direct subtree dump.")
    return parser


def main():
    args = build_parser().parse_args()
    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()
