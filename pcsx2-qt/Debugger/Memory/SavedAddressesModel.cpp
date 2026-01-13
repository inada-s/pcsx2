// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "PrecompiledHeader.h"
#include "SavedAddressesModel.h"

#include "common/Console.h"
#include "DebugTools/ExpressionParser.h"

std::map<BreakPointCpu, SavedAddressesModel*> SavedAddressesModel::s_instances;

SavedAddressesModel::SavedAddressesModel(DebugInterface& cpu, QObject* parent)
	: QAbstractTableModel(parent)
	, m_cpu(cpu)
{
}

SavedAddressesModel* SavedAddressesModel::getInstance(DebugInterface& cpu)
{
	auto iterator = s_instances.find(cpu.getCpuType());
	if (iterator == s_instances.end())
		iterator = s_instances.emplace(cpu.getCpuType(), new SavedAddressesModel(cpu)).first;

	return iterator->second;
}

QString SavedAddressesModel::memorySizeToString(MemorySize size)
{
	switch (size)
	{
		case SIZE_1_BYTE:
			return "1";
		case SIZE_2_BYTES:
			return "2";
		case SIZE_4_BYTES:
			return "4";
		case SIZE_8_BYTES:
			return "8";
		default:
			return "4";
	}
}

SavedAddressesModel::MemorySize SavedAddressesModel::stringToMemorySize(const QString& str)
{
	bool ok;
	int val = str.toInt(&ok);
	if (!ok)
		return SIZE_4_BYTES;

	switch (val)
	{
		case 1:
			return SIZE_1_BYTE;
		case 2:
			return SIZE_2_BYTES;
		case 4:
			return SIZE_4_BYTES;
		case 8:
			return SIZE_8_BYTES;
		default:
			return SIZE_4_BYTES;
	}
}

QString SavedAddressesModel::readMemoryValue(u32 address, MemorySize size) const
{
	if (!m_cpu.isAlive() || !m_cpu.isValidAddress(address))
		return tr("N/A");

	switch (size)
	{
		case SIZE_1_BYTE:
			return QString("%1").arg(m_cpu.read8(address), 2, 16, QChar('0')).toUpper();
		case SIZE_2_BYTES:
			return QString("%1").arg(m_cpu.read16(address), 4, 16, QChar('0')).toUpper();
		case SIZE_4_BYTES:
			return QString("%1").arg(m_cpu.read32(address), 8, 16, QChar('0')).toUpper();
		case SIZE_8_BYTES:
			return QString("%1").arg(m_cpu.read64(address), 16, 16, QChar('0')).toUpper();
		default:
			return QString("%1").arg(m_cpu.read32(address), 8, 16, QChar('0')).toUpper();
	}
}

u32 SavedAddressesModel::evaluateAddress(const QString& expression) const
{
	QString input = expression.trimmed();
	if (input.isEmpty())
		return 0;

	// First try as simple hex number
	bool ok = false;
	u32 address = input.toUInt(&ok, 16);
	if (ok)
		return address;

	// Try parsing as expression (e.g., "0x082000 + 0x0120")
	u64 result;
	std::string error;
	if (m_cpu.evaluateExpression(input.toUtf8().constData(), result, error))
		return static_cast<u32>(result);

	return 0;
}

QVariant SavedAddressesModel::data(const QModelIndex& index, int role) const
{
	size_t row = static_cast<size_t>(index.row());
	if (!index.isValid() || row >= m_savedAddresses.size())
		return QVariant();

	const SavedAddress& entry = m_savedAddresses[row];

	if (role == Qt::CheckStateRole)
		return QVariant();

	if (role == Qt::DisplayRole || role == Qt::EditRole)
	{
		switch (index.column())
		{
			case HeaderColumns::EXPRESSION:
				return entry.expression;
			case HeaderColumns::ADDRESS:
				return QString::number(entry.address, 16).toUpper();
			case HeaderColumns::SIZE:
				return memorySizeToString(entry.size);
			case HeaderColumns::VALUE:
				return readMemoryValue(entry.address, entry.size);
			case HeaderColumns::LABEL:
				return entry.label;
			case HeaderColumns::DESCRIPTION:
				return entry.description;
		}
	}
	if (role == Qt::UserRole)
	{
		switch (index.column())
		{
			case HeaderColumns::EXPRESSION:
				return entry.expression;
			case HeaderColumns::ADDRESS:
				return entry.address;
			case HeaderColumns::SIZE:
				return static_cast<int>(entry.size);
			case HeaderColumns::VALUE:
				return readMemoryValue(entry.address, entry.size);
			case HeaderColumns::LABEL:
				return entry.label;
			case HeaderColumns::DESCRIPTION:
				return entry.description;
		}
	}

	return QVariant();
}

bool SavedAddressesModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
	size_t row = static_cast<size_t>(index.row());
	if (!index.isValid() || row >= m_savedAddresses.size())
		return false;

	SavedAddress& entry = m_savedAddresses[row];

	if (role == Qt::CheckStateRole)
		return false;

	if (role == Qt::EditRole)
	{
		if (index.column() == HeaderColumns::EXPRESSION)
		{
			QString input = value.toString().trimmed();
			entry.expression = input;
			entry.address = evaluateAddress(input);
			// Notify that both EXPRESSION and ADDRESS columns changed
			emit dataChanged(this->index(row, HeaderColumns::EXPRESSION), 
			                 this->index(row, HeaderColumns::ADDRESS), 
			                 QList<int>(role));
			return true;
		}

		if (index.column() == HeaderColumns::SIZE)
			entry.size = stringToMemorySize(value.toString());

		if (index.column() == HeaderColumns::DESCRIPTION)
			entry.description = value.toString();

		if (index.column() == HeaderColumns::LABEL)
			entry.label = value.toString();

		emit dataChanged(index, index, QList<int>(role));
		return true;
	}
	else if (role == Qt::UserRole)
	{
		if (index.column() == HeaderColumns::EXPRESSION)
		{
			entry.expression = value.toString();
			entry.address = evaluateAddress(entry.expression);
		}

		if (index.column() == HeaderColumns::SIZE)
			entry.size = static_cast<MemorySize>(value.toInt());

		if (index.column() == HeaderColumns::DESCRIPTION)
			entry.description = value.toString();

		if (index.column() == HeaderColumns::LABEL)
			entry.label = value.toString();

		emit dataChanged(index, index, QList<int>(role));
		return true;
	}

	return false;
}

QVariant SavedAddressesModel::headerData(int section, Qt::Orientation orientation, int role) const
{
	if (orientation != Qt::Horizontal)
		return QVariant();

	if (role == Qt::DisplayRole)
	{
		switch (section)
		{
			case SavedAddressesModel::EXPRESSION:
				return tr("EXPRESSION");
			case SavedAddressesModel::ADDRESS:
				return tr("ADDRESS");
			case SavedAddressesModel::SIZE:
				return tr("SIZE");
			case SavedAddressesModel::VALUE:
				return tr("VALUE");
			case SavedAddressesModel::LABEL:
				return tr("LABEL");
			case SavedAddressesModel::DESCRIPTION:
				return tr("DESCRIPTION");
			default:
				return QVariant();
		}
	}
	if (role == Qt::UserRole)
	{
		switch (section)
		{
			case SavedAddressesModel::EXPRESSION:
				return "EXPRESSION";
			case SavedAddressesModel::ADDRESS:
				return "ADDRESS";
			case SavedAddressesModel::SIZE:
				return "SIZE";
			case SavedAddressesModel::VALUE:
				return "VALUE";
			case SavedAddressesModel::LABEL:
				return "LABEL";
			case SavedAddressesModel::DESCRIPTION:
				return "DESCRIPTION";
			default:
				return QVariant();
		}
	}
	return QVariant();
}

Qt::ItemFlags SavedAddressesModel::flags(const QModelIndex& index) const
{
	Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
	if (index.column() != HeaderColumns::ADDRESS && index.column() != HeaderColumns::VALUE)
		flags |= Qt::ItemIsEditable;
	return flags;
}

void SavedAddressesModel::addRow()
{
	const SavedAddress defaultNewAddress = {"0", 0, SIZE_4_BYTES, tr("Name"), tr("Description")};
	addRow(defaultNewAddress);
}

void SavedAddressesModel::addRow(SavedAddress addresstoSave)
{
	const int newRowIndex = m_savedAddresses.size();
	beginInsertRows(QModelIndex(), newRowIndex, newRowIndex);
	m_savedAddresses.push_back(addresstoSave);
	endInsertRows();
}

bool SavedAddressesModel::removeRows(int row, int count, const QModelIndex& parent)
{
	if (row < 0 || count < 1 || static_cast<size_t>(row + count) > m_savedAddresses.size())
		return false;

	beginRemoveRows(parent, row, row + count - 1);
	m_savedAddresses.erase(m_savedAddresses.begin() + row, m_savedAddresses.begin() + row + count);
	endRemoveRows();
	return true;
}

int SavedAddressesModel::rowCount(const QModelIndex&) const
{
	return m_savedAddresses.size();
}

int SavedAddressesModel::columnCount(const QModelIndex&) const
{
	return HeaderColumns::COLUMN_COUNT;
}

void SavedAddressesModel::loadSavedAddressFromFieldList(QStringList fields)
{
	if (fields.size() < 3)
	{
		Console.WriteLn("Debugger Saved Addresses Model: Invalid number of columns, skipping");
		return;
	}

	SavedAddressesModel::SavedAddress importedAddress;
	
	// New format: EXPRESSION, ADDRESS, SIZE, VALUE, LABEL, DESCRIPTION (6+ fields)
	// Old format with size: ADDRESS, SIZE, VALUE, LABEL, DESCRIPTION (5 fields)
	// Legacy format: ADDRESS, LABEL, DESCRIPTION (3 fields)
	
	if (fields.size() >= 6)
	{
		// New format with expression
		importedAddress.expression = fields[0];
		importedAddress.address = evaluateAddress(fields[0]);
		importedAddress.size = stringToMemorySize(fields[2]);
		importedAddress.label = fields[4];
		importedAddress.description = fields[5];
	}
	else if (fields.size() >= 5)
	{
		// Old format - use address as expression
		bool ok;
		const u32 address = fields[0].toUInt(&ok, 16);
		if (!ok)
		{
			Console.WriteLn("Debugger Saved Addresses Model: Failed to parse address, skipping");
			return;
		}
		importedAddress.expression = fields[0];
		importedAddress.address = address;
		importedAddress.size = stringToMemorySize(fields[1]);
		importedAddress.label = fields[3];
		importedAddress.description = fields[4];
	}
	else
	{
		// Legacy format
		bool ok;
		const u32 address = fields[0].toUInt(&ok, 16);
		if (!ok)
		{
			Console.WriteLn("Debugger Saved Addresses Model: Failed to parse address, skipping");
			return;
		}
		importedAddress.expression = fields[0];
		importedAddress.address = address;
		importedAddress.size = SIZE_4_BYTES;
		importedAddress.label = fields[1];
		importedAddress.description = fields[2];
	}

	addRow(importedAddress);
}

void SavedAddressesModel::clear()
{
	beginResetModel();
	m_savedAddresses.clear();
	endResetModel();
}

void SavedAddressesModel::refreshData()
{
	if (m_savedAddresses.empty())
		return;

	// Refresh ADDRESS column (expression may contain dynamic values like registers)
	for (size_t i = 0; i < m_savedAddresses.size(); ++i)
	{
		m_savedAddresses[i].address = evaluateAddress(m_savedAddresses[i].expression);
	}

	emit dataChanged(
		index(0, HeaderColumns::ADDRESS),
		index(m_savedAddresses.size() - 1, HeaderColumns::VALUE),
		{Qt::DisplayRole});
}
