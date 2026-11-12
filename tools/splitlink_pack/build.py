"""Builds the M1b SplitLink actor packs from the user's own romfs.

    python -m splitlink_pack.build --romfs <game.nup> --user-dir <emulator user folder>
    python -m splitlink_pack.build --romfs <game.nup> --mod-dir <mod folder>

Writes, under <mod folder>/romfs:
  Pack/Actor/CoopPuppet_Child.pack.zs, CoopPuppet_Adult.pack.zs   (M1a fallback)
  Pack/Actor/SplitLink_Child.pack.zs, SplitLink_Adult.pack.zs     (M1b players)
  RSDB/ActorInfo.Product.<v>.rstbl.byml.zs
  RSDB/GameActorInfo.Product.<v>.rstbl.byml.zs
  System/Resource/ResourceSizeTable.Product.<v>.<platform>.rsizetable.zs
and <mod folder>/splitlink_pack.json describing what it made.

A SplitLink pack keeps every component of the game's own Player_Child or
Player_Adult actor except EventPerformerRef, which would attach the second
Link to the single-player event system. The actor name is still the engine's
`Player` resource for the first build; changing it is a separate experiment.

The code is the co-op project's puppet_pack builder (GPL-3.0), reused from
external/oot-remake-multiplayer/tools so the two tools cannot disagree about
the game's actor tables.
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
COOP_TOOLS = os.path.join(REPO, "external", "oot-remake-multiplayer", "tools")
if not os.path.isdir(os.path.join(COOP_TOOLS, "puppet_pack")):
    # In the portable kit the puppet_pack package sits beside this tool.
    COOP_TOOLS = os.path.dirname(HERE)
if COOP_TOOLS not in sys.path:
    sys.path.insert(0, COOP_TOOLS)

from puppet_pack import build as pp  # noqa: E402
from puppet_pack import byml, dumpcheck, nup, rstb, sarc, zs  # noqa: E402

TITLE_ID = "0400486024DEC000"
DEFAULT_MOD_NAME = "split-link"
VARIANTS = ("Child", "Adult")
PACK_DICT = pp.PACK_DICT
PACK_SIZE_MARGIN = pp.PACK_SIZE_MARGIN

# The event system is single-player: it may claim the second Link and move it
# or hide it. Everything else of the player is kept, including PlayerRef,
# BSARef, PhysicsRef, LifeRef, ConditionRef and the camera target.
DROP_COMPONENTS = ("EventPerformerRef",)
# The engine refuses a second actor named Player. Dm_Npc_Base is an existing
# engine actor name used by the player-model demo actors, so the second Link
# keeps the player components but is not the singleton Player.
ACTOR_NAME_REF = "?ActorSystem/ActorName/Dm_Npc_Base.engine__actor__ActorName.bgyml"

# CoopLink: the draw-only puppet plus the components that give it a real
# body and a Life pool, without the PlayerRef singleton. This is the middle
# ground between the puppet and the refused full player.
DEFAULT_COOPLINK_KEEP = (
    "ActorHolderRef",
    "ActorPositionCalculatorRef",
    "AffiliationRef",
    "AttentionReceiverRef",
    "BlackboardRef",
    "ChemicalRef",
    "ConditionRef",
    "DeathRef",
    "GameBalanceClientRef",
    "GameLifeConditionRef",
    "LifeRef",
    "PhysicsRef",
    "ReactionRef",
    "SensorHitRef",
    "TargetRef",
)
# LICHII_COOPLINK_EXTRA: comma-separated subset for bring-up tests.
COOPLINK_KEEP = pp.KEEP_COMPONENTS + tuple(
    filter(None, os.environ.get("LICHII_COOPLINK_EXTRA",
                                ",".join(DEFAULT_COOPLINK_KEEP)).split(",")))


def splitlink_name(variant):
    return f"SplitLink_{variant}"


def splitlink_param(player_components):
    # LICHII_SPLITLINK_KEEP: comma-separated extra components for bring-up
    # tests (the model/AS/equipment base is always kept). Unset keeps every
    # player component except the event performer.
    extra = os.environ.get("LICHII_SPLITLINK_KEEP")
    if extra:
        keep = pp.KEEP_COMPONENTS + tuple(filter(None, extra.split(",")))
        comps = {k: v for k, v in player_components.items() if k in keep}
    else:
        comps = {k: v for k, v in player_components.items() if k not in DROP_COMPONENTS}
    comps["ActorNameRef"] = ACTOR_NAME_REF
    return {"Components": comps}


def build_splitlink(members, variant):
    """Returns (pack bytes, actor param bytes, member list, equipment keys)."""
    player = f"Player_{variant}"
    actor = splitlink_name(variant)
    doc = splitlink_param(pp.flatten_actor_param(members, player))
    files = pp.collect_members(members, doc)
    keys = pp.equipment_keys(files, doc)
    ap = byml.dumps(doc)
    files[pp.actor_param_path(actor)] = ap
    return sarc.write(files), ap, sorted(files), keys


def cooplink_param(player_components):
    comps = {k: v for k, v in player_components.items() if k in COOPLINK_KEEP}
    comps.update(pp.FIXED_COMPONENTS)
    # A valid actor-name row is needed once Physics/Life are present.
    comps["ActorNameRef"] = ACTOR_NAME_REF
    # Use the cutscene player body that the engine already accepts; the
    # player's own controller set is reserved for the singleton Player.
    comps["PhysicsRef"] = "?Component/Physics/Player_Child_Op.engine__component__PhysicsParam.bgyml"
    return {"Components": comps}


def build_cooplink(members, variant, dm_members):
    """Returns (pack bytes, actor param bytes, member list, equipment keys)."""
    player = f"Player_{variant}"
    actor = f"CoopLink_{variant}"
    merged = dict(dm_members)
    merged.update(members)
    doc = cooplink_param(pp.flatten_actor_param(merged, player))
    files = pp.collect_members(merged, doc)
    keys = pp.equipment_keys(files, doc)
    ap = byml.dumps(doc)
    files[pp.actor_param_path(actor)] = ap
    return sarc.write(files), ap, sorted(files), keys


def make_rows(ai_rows, gai_rows, actor, full_player):
    if actor not in ai_rows:
        raise pp.BuildError(f"ActorInfo has no row {actor}")
    if full_player:
        # A real player keeps the model, ELink and SLink user names. Without
        # them the actor management request is dropped before creation.
        row = dict(ai_rows[actor])
        row["__RowId"] = actor
        # Not the singleton Player: the model names stay, the actor-name
        # lookup key becomes the existing Dm_Npc_Base name.
        row["ActorName"] = "Dm_Npc_Base"
        gai = dict(gai_rows.get(actor, {}))
        gai["__RowId"] = actor
        return row, gai
    row = pp.info_row(ai_rows[actor], actor)
    return row, {"__RowId": actor}


def build(romfs, mod_dir, log=print):
    if not os.path.isfile(os.path.join(romfs, "Pack", "ZsDic.pack.zs")):
        raise pp.BuildError(f"{romfs} does not look like the game's romfs (no Pack/ZsDic.pack.zs)")
    dicts = zs.Dicts.from_romfs(romfs)
    if PACK_DICT not in dicts.by_name:
        raise pp.BuildError(f"Pack/ZsDic.pack.zs has no {PACK_DICT}")
    out_romfs = os.path.join(mod_dir, "romfs")

    ai_path = pp.find_one(romfs, "RSDB/ActorInfo.Product.*.rstbl.byml.zs")
    gai_path = pp.find_one(romfs, "RSDB/GameActorInfo.Product.*.rstbl.byml.zs")
    rs_path = pp.find_one(romfs, "System/Resource/ResourceSizeTable.Product.*.rsizetable.zs")
    ai_old, gai_old, rs_old = pp.read(ai_path), pp.read(gai_path), pp.read(rs_path)
    ai = byml.loads(dicts.decompress(ai_old))
    gai = byml.loads(dicts.decompress(gai_old))
    table = rstb.Rstb.loads(dicts.decompress(rs_old))
    ai_rows = {r["__RowId"]: r for r in ai}
    gai_rows = {r["__RowId"]: r for r in gai}

    made = []
    new_ai, new_gai = [], []
    for variant in VARIANTS:
        player = f"Player_{variant}"
        src = os.path.join(romfs, "Pack", "Actor", f"{player}.pack.zs")
        if not os.path.isfile(src):
            raise pp.BuildError(f"missing Pack/Actor/{player}.pack.zs")
        members = sarc.read(dicts.decompress(pp.read(src)))
        dm_src = os.path.join(romfs, "Pack", "Actor", "Dm_Player_Child_Op.pack.zs")
        dm_members = {}
        if os.path.isfile(dm_src):
            dm_members = sarc.read(dicts.decompress(pp.read(dm_src)))

        # M1a fallback: draw-only pack, as the co-op project makes it.
        coop_actor = pp.puppet_name(variant)
        coop_pack, coop_ap, coop_names, coop_keys = pp.build_pack(members, variant)
        # The puppet plus Life and Physics, still without PlayerRef.
        cooplink_actor = f"CoopLink_{variant}"
        cooplink_pack, cooplink_ap, cooplink_names, cooplink_keys = build_cooplink(
            members, variant, dm_members)
        # M1b player: every player component except the event performer.
        actor = splitlink_name(variant)
        pack, ap, names, keys = build_splitlink(members, variant)

        for name, data, ap_data, prefix in (
            (coop_actor, coop_pack, coop_ap, "CoopPuppet"),
            (cooplink_actor, cooplink_pack, cooplink_ap, "CoopLink"),
            (actor, pack, ap, "SplitLink"),
        ):
            dst_rel = f"Pack/Actor/{name}.pack.zs"
            pp.write(os.path.join(out_romfs, dst_rel), dicts.compress(data, PACK_DICT))
            # Every mod actor gets the full player row with its own
            # ELink/SLink users and without the singleton Player affiliation.
            # The draw-only row (no ActorName/ELink/SLink) is dropped by the
            # actor manager before creation.
            ai_row, gai_row = make_rows(ai_rows, gai_rows, player, True)
            ai_row["__RowId"] = name
            gai_row["__RowId"] = name
            ai_row["ELinkUserName"] = name
            ai_row["SLinkUserName"] = name
            ai_row["CoreUsedLevel"] = pp.byml.UInt(0)
            # The player row asks for a 10 MB instance heap. The actor
            # manager refuses a second actor at that size (state 4); the
            # cutscene player row uses 0 and creates.
            ai_row["InstanceHeapSize"] = 0
            gai_row.pop("DefaultAffiliation", None)
            gai_row.pop("MaxLife", None)
            new_ai.append(ai_row)
            new_gai.append(gai_row)
            table.add_new(pp.strip_zs(dst_rel), ((len(data) + 0x1F) & ~0x1F) + PACK_SIZE_MARGIN)
            player_ap = table.get(pp.actor_param_path(player))
            if player_ap is None:
                raise pp.BuildError(f"the size table has no {pp.actor_param_path(player)}")
            table.add_new(pp.actor_param_path(name), max(player_ap, len(ap_data) + PACK_SIZE_MARGIN))
            made.append({
                "actor": name,
                "from": player,
                "pack": dst_rel,
                "pack_bytes": len(data),
                "members": {
                    "CoopPuppet": coop_names,
                    "CoopLink": cooplink_names,
                    "SplitLink": names,
                }[prefix],
                "equipment_keys": {
                    "CoopPuppet": coop_keys,
                    "CoopLink": cooplink_keys,
                    "SplitLink": keys,
                }[prefix],
            })
            log(f"{name}: {len(made[-1]['members'])} files, {len(data)} bytes -> {dst_rel}")

    ai_new, gai_new = byml.dumps(pp.upsert_rows(ai, new_ai)), byml.dumps(pp.upsert_rows(gai, new_gai))
    for path, old, new in ((ai_path, ai_old, ai_new), (gai_path, gai_old, gai_new)):
        key = pp.strip_zs(pp.rel(romfs, path))
        size = table.get(key)
        if size is None:
            raise pp.BuildError(f"the size table has no {key}")
        table.set(key, size + max(0, len(new) - len(old)) + PACK_SIZE_MARGIN)
    for path, new in ((ai_path, ai_new), (gai_path, gai_new), (rs_path, table.dumps())):
        r = pp.rel(romfs, path)
        pp.write(os.path.join(out_romfs, r), pp.recompress(dicts, pp.read(path), new))
        log(f"wrote {r}")

    manifest = {
        "tool": "tools/splitlink_pack",
        "title_id": TITLE_ID,
        "actors": made,
        "tables": [pp.rel(romfs, p) for p in (ai_path, gai_path, rs_path)],
        "source_sha256": {pp.rel(romfs, p): hashlib.sha256(pp.read(p)).hexdigest()
                          for p in (ai_path, gai_path, rs_path)},
    }
    pp.write(os.path.join(mod_dir, "splitlink_pack.json"), json.dumps(manifest, indent=1).encode())
    return manifest


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--romfs", required=True, help="the game's .nup file, or its extracted romfs folder")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--mod-dir", help="the mod folder to write (its romfs/ is made inside)")
    g.add_argument("--user-dir", help="the emulator's user folder; writes load/<title id>/<mod name>")
    ap.add_argument("--mod-name", default=DEFAULT_MOD_NAME)
    ap.add_argument("--any-version", action="store_true",
                    help=f"accept a dump that is not game data version {dumpcheck.EXPECTED_VERSION}")
    a = ap.parse_args(argv)
    from_nup = os.path.isfile(a.romfs) and nup.is_nup(a.romfs)
    if from_nup:
        problems = dumpcheck.check_nup(a.romfs, a.any_version)
    else:
        a.romfs = dumpcheck.resolve(a.romfs)
        problems = dumpcheck.check_romfs(a.romfs, a.any_version)
    if problems:
        for p in problems:
            print(f"error: {p}", file=sys.stderr)
        return 1
    if from_nup:
        tmp = tempfile.mkdtemp(prefix="splitlink-puppet-")
        try:
            with nup.Nup(a.romfs) as n:
                for rel in dumpcheck.nup_files(n).values():
                    nup.write_under(tmp, rel, n.read(rel))
                # The cutscene player body used by the CoopLink physics ref.
                dm_rel = "Pack/Actor/Dm_Player_Child_Op.pack.zs"
                try:
                    nup.write_under(tmp, dm_rel, n.read(dm_rel))
                except nup.NupError:
                    pass
            print(f"read the game files from {a.romfs}")
            return _main_build(a, tmp)
        except nup.NupError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    return _main_build(a, a.romfs)


def _main_build(a, romfs):
    if a.mod_dir:
        mod_dir = a.mod_dir
    else:
        root = pp.load_root(a.user_dir)
        if os.path.normcase(root) != os.path.normcase(os.path.join(a.user_dir, "load")):
            print(f"note: the emulator's settings load mods from {root}")
        mod_dir = os.path.join(root, TITLE_ID, a.mod_name)
    try:
        build(romfs, mod_dir)
    except pp.BuildError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(f"done: {mod_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
