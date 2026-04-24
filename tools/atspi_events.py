#!/usr/bin/env -S uv run --script
# /// script
# dependencies = ["dbus-next"]
# ///

from __future__ import annotations

import argparse
import asyncio

from dbus_next import Message
from dbus_next.aio import MessageBus
from dbus_next.constants import BusType, MessageType


EVENT_NAME_BY_SIGNAL = {
    "TextCaretMoved": "object:text-caret-moved",
    "TextSelectionChanged": "object:text-selection-changed",
    "StateChanged": "object:state-changed",
}


async def get_atspi_address() -> str:
    bus = await MessageBus(bus_type=BusType.SESSION).connect()
    intro = await bus.introspect("org.a11y.Bus", "/org/a11y/bus")
    obj = bus.get_proxy_object("org.a11y.Bus", "/org/a11y/bus", intro)
    iface = obj.get_interface("org.a11y.Bus")
    return await iface.call_get_address()


async def main_async(args: argparse.Namespace):
    bus = await MessageBus(bus_address=await get_atspi_address()).connect()
    registry_intro = await bus.introspect("org.a11y.atspi.Registry", "/org/a11y/atspi/registry")
    registry = bus.get_proxy_object(
        "org.a11y.atspi.Registry", "/org/a11y/atspi/registry", registry_intro
    ).get_interface("org.a11y.atspi.Registry")
    registered = args.event or ["TextCaretMoved"]
    if args.all_object_events:
        registered = ["object:"]
    for event in registered:
        await registry.call_register_event(EVENT_NAME_BY_SIGNAL.get(event, event), [], "")

    match = "type='signal',interface='org.a11y.atspi.Event.Object'"
    reply = await bus.call(
        Message(
            destination="org.freedesktop.DBus",
            path="/org/freedesktop/DBus",
            interface="org.freedesktop.DBus",
            member="AddMatch",
            signature="s",
            body=[match],
        )
    )
    if reply.message_type == MessageType.ERROR:
        raise SystemExit(reply.body)

    wanted = set(args.event or [])

    def on_message(message: Message):
        if message.message_type != MessageType.SIGNAL:
            return
        if message.interface != "org.a11y.atspi.Event.Object":
            return
        if wanted and message.member not in wanted:
            return
        body = message.body or []
        print(f"{message.sender} {message.path} {message.member} {body}", flush=True)

    bus.add_message_handler(on_message)
    await asyncio.sleep(args.seconds)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Trace AT-SPI object events.")
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--event", action="append", help="Signal name to include, e.g. TextCaretMoved.")
    parser.add_argument("--all-object-events", action="store_true", help="Register for all AT-SPI object events.")
    return parser


def main():
    asyncio.run(main_async(build_parser().parse_args()))


if __name__ == "__main__":
    main()
