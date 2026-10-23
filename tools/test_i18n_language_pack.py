import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import i18n_language_pack as i18n


class I18nLanguagePackTests(unittest.TestCase):
    def test_digit_separators_do_not_swallow_later_comments_and_ui(self) -> None:
        for number in ("10'000", "1'000'000", "0xDEAD'BEEF", "0b1010'1111", "1.5'001", ".5'001"):
            with self.subTest(number=number):
                source = (f"auto timeout = {number};\n"
                          "// QLayout's delayed show is unrelated to string tokens.\n"
                          'label.setText(QStringLiteral("分隔符后的说明"));\n'
                          "auto digit = QChar('0');\n"
                          'label.setText("另一个字段");\n')
                self.assertEqual(
                    [("分隔符后的说明", 3), ("另一个字段", 5)],
                    list(i18n.extract_cpp_literals(source)),
                )

    def test_character_literals_after_numeric_values_still_skip_quotes(self) -> None:
        source = ("auto a = 1 + '8'; auto b = u'0'; auto c = L'1'; auto d = u8'a';\n"
                  'label.setText("保留原文字段");\n')
        self.assertEqual([("保留原文字段", 2)], list(i18n.extract_cpp_literals(source)))

    def test_include_header_names_are_not_ui_literals(self) -> None:
        source = ('#include "FieldTreeModel.h"\n'
                  '  # include /* header */ "TableRowFilterSupport.h"\n'
                  '#include <TableRowFilterIdentityState.h>\n'
                  '#include \\\n "中文头文件.h"\n'
                  '/* leading comment */ #include "SameName.h"\n'
                  'label.setText("SameName.h");\n')
        self.assertEqual([("SameName.h", 7)], list(i18n.extract_cpp_literals(source)))
        self.assertEqual([], list(i18n.extract_cpp_concatenated_literals(source)))

    def test_include_looking_ui_text_and_other_directives_still_audit(self) -> None:
        source = ('#define UI_LABEL "FieldTreeModel.h"\n'
                  'label.setText("TableRowFilterSupport.h");\n'
                  'label.setText("TableRowFilterIdentityState.h");\n'
                  'label.setText(R"(#include "中文正文.h")");\n')
        values = dict(i18n.extract_cpp_literals(source))
        for text in ("FieldTreeModel.h", "TableRowFilterSupport.h",
                     "TableRowFilterIdentityState.h", '#include "中文正文.h"'):
            self.assertIn(text, values)
        missing = i18n.audit(
            {text: i18n.ExtractedString(text) for text in values},
            {"translations": {}, "context_translations": {}, "source_translations": {}},
            {"translations": {}, "context_translations": {}, "source_translations": {}},
        )
        for text in ("FieldTreeModel.h", "TableRowFilterSupport.h", "TableRowFilterIdentityState.h"):
            for language in ("zh-CN", "en-US"):
                self.assertIn(f"missing {language} source translation: {text!r}", missing)

    def test_hash_after_code_is_not_an_include_directive(self) -> None:
        source = 'int value; #include "BodyText.h"\n'
        self.assertEqual([("BodyText.h", 1)], list(i18n.extract_cpp_literals(source)))

    def test_extracts_compiler_concatenated_literal(self) -> None:
        source = '''
QString value = QStringLiteral(
    "第一段：%1"
    /* C++ still concatenates across comments. */
    "；第二段：%2");
'''
        self.assertIn(
            ("第一段：%1；第二段：%2", 3),
            list(i18n.extract_cpp_concatenated_literals(source)),
        )

    def test_does_not_join_literals_across_punctuation(self) -> None:
        source = 'QStringList values{QStringLiteral("甲"), QStringLiteral("乙")};'
        self.assertEqual([], list(i18n.extract_cpp_concatenated_literals(source)))

    def test_extracts_referenced_semantic_keys(self) -> None:
        source = '''
language.bindText(label,
    QStringLiteral("settings.feature.label"),
    QStringLiteral("功能"));
translated("dialog.action.accept", "确定");
'''
        self.assertEqual(
            [("settings.feature.label", 3), ("dialog.action.accept", 5)],
            list(i18n.extract_semantic_key_references(source)),
        )

    def test_audit_rejects_missing_referenced_key(self) -> None:
        occurrence = i18n.Occurrence("Page.cpp", 42)
        errors = i18n.audit(
            {},
            {"translations": {}, "context_translations": {}, "source_translations": {}},
            {"translations": {}, "context_translations": {}, "source_translations": {}},
            {"page.action.run": [occurrence]},
        )
        self.assertIn(
            "missing zh-CN referenced semantic translation: 'page.action.run' (Page.cpp:42)",
            errors,
        )
        self.assertIn(
            "missing en-US referenced semantic translation: 'page.action.run' (Page.cpp:42)",
            errors,
        )


if __name__ == "__main__":
    unittest.main()
