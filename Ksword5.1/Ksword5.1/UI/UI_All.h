#pragma once

#include <QWidget>

// createBasicPlaceholder:
// - Inputs: tipText is the text displayed in the placeholder body.
// - Processing: the implementation builds a simple QWidget with centered text.
// - Return: a newly allocated QWidget owned by the caller/Qt parent chain.
QWidget* createBasicPlaceholder(const QString& tipText = "Placeholder panel");
