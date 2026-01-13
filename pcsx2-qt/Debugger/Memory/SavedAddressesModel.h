// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <QtCore/QAbstractTableModel>
#include <QtWidgets/QHeaderView>

#include "DebugTools/DebugInterface.h"

class SavedAddressesModel : public QAbstractTableModel
{
	Q_OBJECT

public:
	enum MemorySize : int
	{
		SIZE_1_BYTE = 0,
		SIZE_2_BYTES,
		SIZE_4_BYTES,
		SIZE_8_BYTES,
		SIZE_COUNT
	};

	struct SavedAddress
	{
		QString expression;
		u32 address;
		MemorySize size = SIZE_4_BYTES;
		QString label;
		QString description;
	};

	enum HeaderColumns : int
	{
		EXPRESSION = 0,
		ADDRESS,
		SIZE,
		VALUE,
		LABEL,
		DESCRIPTION,
		COLUMN_COUNT
	};

	static constexpr QHeaderView::ResizeMode HeaderResizeModes[HeaderColumns::COLUMN_COUNT] = {
		QHeaderView::ResizeMode::ResizeToContents,
		QHeaderView::ResizeMode::ResizeToContents,
		QHeaderView::ResizeMode::ResizeToContents,
		QHeaderView::ResizeMode::ResizeToContents,
		QHeaderView::ResizeMode::ResizeToContents,
		QHeaderView::ResizeMode::Stretch,
	};

	static SavedAddressesModel* getInstance(DebugInterface& cpu);

	QVariant data(const QModelIndex& index, int role) const override;
	QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
	int rowCount(const QModelIndex& parent = QModelIndex()) const override;
	int columnCount(const QModelIndex& parent = QModelIndex()) const override;
	Qt::ItemFlags flags(const QModelIndex& index) const override;
	void addRow();
	void addRow(SavedAddress addresstoSave);
	bool removeRows(int row, int count, const QModelIndex& parent = QModelIndex()) override;
	bool setData(const QModelIndex& index, const QVariant& value, int role) override;
	void loadSavedAddressFromFieldList(QStringList fields);
	void clear();
	void refreshData();

	static QString memorySizeToString(MemorySize size);
	static MemorySize stringToMemorySize(const QString& str);

private:
	SavedAddressesModel(DebugInterface& cpu, QObject* parent = nullptr);

	QString readMemoryValue(u32 address, MemorySize size) const;
	u32 evaluateAddress(const QString& expression) const;

	DebugInterface& m_cpu;
	std::vector<SavedAddress> m_savedAddresses;

	static std::map<BreakPointCpu, SavedAddressesModel*> s_instances;
};
