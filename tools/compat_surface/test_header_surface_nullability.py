import unittest

import header_surface


class HeaderSurfaceNullabilityTests(unittest.TestCase):
    @staticmethod
    def _objc_pointer(spelling: str) -> dict:
        return {
            "kind": "ObjCObjectPointerType",
            "type": {"qualType": spelling},
            "inner": [
                {
                    "kind": "ObjCObjectType",
                    "type": {"qualType": spelling.replace(" _Nullable", "")},
                }
            ],
        }

    def test_objc_nullability_is_not_a_mechanical_type_distinction(self):
        base = self._objc_pointer("id")
        nullable = {
            "kind": "AttributedType",
            "type": {"qualType": "id _Nullable"},
            "inner": [base],
        }

        self.assertEqual(
            header_surface._type_descriptor(nullable),
            header_surface._type_descriptor(base),
        )

    def test_other_attributed_types_remain_explicit(self):
        pointer = {
            "kind": "PointerType",
            "type": {"qualType": "int *"},
            "inner": [
                {"kind": "BuiltinType", "type": {"qualType": "int"}}
            ],
        }
        attributed = {
            "kind": "AttributedType",
            "type": {"qualType": "int * __attribute__((address_space(1)))"},
            "inner": [pointer],
        }

        descriptor = header_surface._type_descriptor(attributed)
        self.assertEqual(descriptor["kind"], "attributed")
        self.assertEqual(descriptor["base"], header_surface._type_descriptor(pointer))

    def test_old_and_nullable_objc_redeclarations_merge_mechanically(self):
        id_plain = header_surface._type_descriptor(self._objc_pointer("id"))
        id_nullable = header_surface._type_descriptor(
            {
                "kind": "AttributedType",
                "type": {"qualType": "id _Nullable"},
                "inner": [self._objc_pointer("id")],
            }
        )
        class_plain = header_surface._type_descriptor(self._objc_pointer("Class"))
        class_nullable = header_surface._type_descriptor(
            {
                "kind": "AttributedType",
                "type": {"qualType": "Class _Nullable"},
                "inner": [self._objc_pointer("Class")],
            }
        )
        size_t = {"kind": "builtin", "name": "unsigned long"}

        old = header_surface.HeaderSignature(
            header="usr/include/objc/objc-auto.h",
            name="class_createInstance",
            symbol="_class_createInstance",
            function_type="id (Class, size_t)",
            calling_convention="cdecl",
            variadic=False,
            prototype=True,
            return_type=id_plain,
            parameter_types=(class_plain, size_t),
            params=(
                header_surface.RawParam("cls", "Class"),
                header_surface.RawParam("extraBytes", "size_t"),
            ),
            line=241,
            column=16,
        )
        modern = header_surface.HeaderSignature(
            header="usr/include/objc/runtime.h",
            name="class_createInstance",
            symbol="_class_createInstance",
            function_type=(
                "OBJC_RETURNS_RETAINED id _Nullable "
                "(Class _Nullable, size_t)"
            ),
            calling_convention="cdecl",
            variadic=False,
            prototype=True,
            return_type=id_nullable,
            parameter_types=(class_nullable, size_t),
            params=(
                header_surface.RawParam("cls", "Class _Nullable"),
                header_surface.RawParam("extraBytes", "size_t"),
            ),
            line=863,
            column=1,
        )

        manifest = header_surface.build_manifest(
            [old, modern],
            target=header_surface.DEFAULT_TARGET,
            headers=[old.header, modern.header],
        )

        self.assertEqual(manifest["summary"]["declaration_count"], 2)
        self.assertEqual(manifest["summary"]["unique_symbol_count"], 1)
        signature = manifest["signatures"][0]
        self.assertEqual(signature["symbol"], "_class_createInstance")
        self.assertEqual(signature["return_type"], id_plain)
        self.assertEqual(signature["parameters"][0]["type"], class_plain)
        self.assertEqual(
            signature["function_type_spellings"],
            sorted([old.function_type, modern.function_type]),
        )
        self.assertEqual(
            [source["header"] for source in signature["sources"]],
            [old.header, modern.header],
        )


if __name__ == "__main__":
    unittest.main()
