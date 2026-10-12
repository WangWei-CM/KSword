from __future__ import annotations

import unittest
from pathlib import Path

import ksword_profile_release_sync as release_sync
import ksword_pdb_profile_generator as generator


class V4OnlyPackTests(unittest.TestCase):
    def make_record(
        self,
        name: str,
        field_count: int,
        typed_item_count: int,
        v4_item_count: int,
    ) -> release_sync.ProfileRecord:
        return release_sync.ProfileRecord(
            path=Path(f"{name}.json"),
            data={
                "profileName": name,
                "fields": {},
                "module": {
                    "class": "ntoskrnl",
                    "machine": 0x8664,
                    "timeDateStamp": 1,
                    "sizeOfImage": 1,
                },
            },
            identity=release_sync.ProfileIdentity("ntoskrnl", 0x8664, 1, 1),
            field_count=field_count,
            typed_items=[{"name": "EpUniqueProcessId", "kind": "StructOffset", "value": 0x440}]
            * typed_item_count,
            v4_items=[
                {
                    "itemId": 58,
                    "name": "EpUniqueProcessId",
                    "itemKind": 1,
                    "kind": "StructOffset",
                    "flags": 1,
                    "capabilityGroupId": 1,
                    "value": 0x1000,
                    "aux0": 0,
                    "aux1": 0,
                    "aux2": 0,
                    "aux3": 0,
                }
            ]
            * v4_item_count,
        )

    def test_pack_version_filters_profiles_by_supported_payload(self) -> None:
        legacy = self.make_record("legacy", 1, 0, 0)
        typed = self.make_record("typed", 0, 1, 0)
        v4_only = self.make_record("v4_only", 0, 0, 1)
        records = [legacy, typed, v4_only]

        self.assertEqual(
            release_sync.KSW_SUPPORTED_PACK_VERSIONS,
            (release_sync.KSW_PACK_VERSION_V4,),
        )
        self.assertEqual(
            [typed, v4_only],
            release_sync.records_for_pack_version(records, release_sync.KSW_PACK_VERSION_V4),
        )
        self.assertEqual(release_sync.KSW_PACK_VERSION_V4, release_sync.KSW_DEFAULT_PACK_VERSION)
        with self.assertRaisesRegex(ValueError, "unsupported pack version"):
            release_sync.records_for_pack_version(records, 1)
        with self.assertRaisesRegex(ValueError, "unsupported pack version"):
            release_sync.records_for_pack_version(records, 2)

    def test_v4_entry_has_no_legacy_offset_mirrors(self) -> None:
        record = self.make_record("v4", 1, 0, 1)
        entry = release_sync.build_pack_profile_entry(record, pack_version=release_sync.KSW_PACK_VERSION_V4)
        self.assertIn("items", entry)
        self.assertNotIn("fields", entry)
        self.assertNotIn("legacyItems", entry)
        self.assertNotIn("callbackItems", entry)

    def test_reserved_timer_ids_are_not_generated(self) -> None:
        self.assertNotIn(1004, release_sync.V4_SPECIAL_ITEM_IDS.values())
        self.assertNotIn(1006, release_sync.V4_SPECIAL_ITEM_IDS.values())
        self.assertEqual(
            release_sync.V4_FIXED_CAPABILITY_GROUP_COUNTS[release_sync.V4_TIMER_GROUP_ID],
            (15, 0),
        )

    def test_optional_psp_symbol_survives_pack_without_becoming_required(self) -> None:
        record = self.make_record("psp", 0, 0, 0)
        raw = {
            "name": "PspTerminateProcess", "itemId": 1401, "kind": "GlobalRva",
            "flags": "optional", "capabilityGroupId": 1, "value": "0x123456",
        }
        state = release_sync.ValidationState()
        normalized = release_sync.validate_v4_items(record.path, {"v4Items": [raw]}, state)
        self.assertIsNotNone(normalized)
        record.v4_items = normalized
        packed = release_sync.build_pack_profile_entry(record)
        item = packed["items"][0]
        self.assertEqual(item["itemId"], 1401)
        self.assertEqual(item["itemKind"], release_sync.V4_ITEM_KIND_IDS["GlobalRva"])
        self.assertEqual(item["flags"], 2)
        self.assertEqual(item["capabilityGroupId"], 1)
        self.assertEqual(item["valueLow"], 0x123456)
        group = packed["capabilityGroups"][0]
        self.assertEqual(group["requiredItemCount"], 0)
        self.assertEqual(group["optionalItemCount"], 1)

    def test_psp_symbol_is_optional_and_limited_to_kernel_profiles(self) -> None:
        self.assertIn("PspTerminateProcess", generator.V4_OPTIONAL_ITEM_NAMES)
        self.assertEqual(generator.V4_ITEM_DEFINITIONS["PspTerminateProcess"], (1401, "GlobalRva", 1))
        for module in ("ntoskrnl", "ntkrla57"):
            self.assertIn(1, generator.v4_group_ids_for_module(module))
        for module in ("ci", "fltmgr", "win32k"):
            self.assertNotIn(1, generator.v4_group_ids_for_module(module))


class ProtocolAndCoreExtractionTests(unittest.TestCase):
    def test_protocol_catalog_accepts_psp_and_rejects_retired_or_unknown_ids(self):
        self.assertIn(1401, release_sync.SUPPORTED_V4_ITEM_IDS)
        self.assertNotIn(1402, release_sync.SUPPORTED_V4_ITEM_IDS)
        self.assertNotIn(1004, release_sync.SUPPORTED_V4_ITEM_IDS)
        for name, definition in generator.V4_ITEM_DEFINITIONS.items():
            self.assertEqual(definition[0], release_sync.V4_SPECIAL_ITEM_IDS[name])
            self.assertIn(definition[0], release_sync.SUPPORTED_V4_ITEM_IDS)

    def test_nested_union_resolves_by_type_id_and_missing_member_stays_missing(self):
        types = """
   0x1000 | LF_STRUCTURE [size = 32] `_SECTION`
            field list: 0x1001
            options: sizeof 48
   0x1001 | LF_FIELDLIST [size = 24]
            - LF_MEMBER [name = `u1`, Type = 0x1002, offset = 40, attrs = public]
   0x1002 | LF_UNION [size = 24] `<anonymous-tag>`
            field list: 0x1003
            options: sizeof 8
   0x1003 | LF_FIELDLIST [size = 24]
            - LF_MEMBER [name = `ControlArea`, Type = 0x0012, offset = 0, attrs = public]
   0x1004 | LF_UNION [size = 24] `<anonymous-tag>`
            field list: 0x1005
            options: sizeof 8
   0x1005 | LF_FIELDLIST [size = 24]
            - LF_MEMBER [name = `ControlArea`, Type = 0x0012, offset = 16, attrs = public]
"""
        offsets, _, _ = generator.resolve_type_layouts_batch(types,
            [("_SECTION", "u1.ControlArea"), ("_SECTION", "u1.Missing")], [], [])
        self.assertEqual(offsets[("_SECTION", "u1.ControlArea")], 40)
        self.assertNotIn(("_SECTION", "u1.Missing"), offsets)

    def test_handle_shifts_use_bit_positions_and_reject_incompatible_metadata(self):
        bits = {("_HANDLE_TABLE_ENTRY", "ObjectPointerBits"): (0, 20, 44, 8),
                ("_HANDLE_TABLE_ENTRY", "Attributes"): (0, 17, 3, 8)}
        fields, _ = generator.resolve_core_fields({}, bits)
        self.assertEqual(fields["ObDecodeShift"], "0x0014")
        self.assertEqual(fields["ObAttributesShift"], "0x0011")
        bits[("_HANDLE_TABLE_ENTRY", "ObjectPointerBits")] = (0, 20, 40, 8)
        fields, missing = generator.resolve_core_fields({}, bits)
        self.assertNotIn("ObDecodeShift", fields)
        self.assertIn("_HANDLE_TABLE_ENTRY->ObjectPointerBits", missing)


if __name__ == "__main__":
    unittest.main()
