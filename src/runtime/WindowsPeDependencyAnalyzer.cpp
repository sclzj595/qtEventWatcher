#include "PeDependencyAnalyzer.h"

#include "ModuleEnumerator.h"

#ifdef Q_OS_WIN

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLibraryInfo>

namespace qt_event_watcher
{

namespace {

/// RVA → 文件偏移；落在 BSS（无原始数据映射）或未命中任何 section 返回 0
quint32 rvaToOffset(const IMAGE_SECTION_HEADER* sections, int count, quint32 rva)
{
	for (int i = 0; i < count; ++i) {
		const IMAGE_SECTION_HEADER& s = sections[i];
		const quint32 vsize = qMax(s.Misc.VirtualSize, s.SizeOfRawData);
		if (rva >= s.VirtualAddress && rva < s.VirtualAddress + vsize) {
			const quint32 delta = rva - s.VirtualAddress;
			if (delta >= s.SizeOfRawData)
				return 0;	// 位于未初始化数据段，文件中不存在
			return s.PointerToRawData + delta;
		}
	}
	return 0;
}

/// 从文件偏移处读 NUL 结尾 ASCII 字符串（DLL 名均为 ASCII），带边界保护
QString readCString(const QByteArray& pe, quint32 offset, quint32 maxLen)
{
	if (offset == 0 || offset >= quint32(pe.size()))
		return {};
	const char* p = pe.constData() + offset;
	const quint32 limit = qMin<quint32>(maxLen, quint32(pe.size()) - offset);
	quint32 len = 0;
	while (len < limit && p[len] != '\0')
		++len;
	return QString::fromLatin1(p, int(len));
}

} // namespace

QStringList PeDependencyAnalyzer::readImports(const QString& exePath, bool& ok, QString& failReason)
{
	QStringList imports;
	ok = false;

	QFile file(exePath);
	if (!file.open(QIODevice::ReadOnly)) {
		failReason = QStringLiteral("cannot open %1").arg(exePath);
		return imports;
	}
	const QByteArray pe = file.readAll();
	file.close();

	if (pe.size() < int(sizeof(IMAGE_DOS_HEADER))) {
		failReason = QStringLiteral("file too small");
		return imports;
	}

	IMAGE_DOS_HEADER dos;
	memcpy(&dos, pe.constData(), sizeof(dos));
	if (dos.e_magic != 0x5A4D) {	// 'MZ'
		failReason = QStringLiteral("not a PE file (bad MZ signature)");
		return imports;
	}

	const qint64 peOff = dos.e_lfanew;
	const qint64 headerEnd = peOff + 4 + static_cast<qint64>(sizeof(IMAGE_FILE_HEADER));	// 全 qint64 消 C4018（V6 Q2）
	if (peOff <= 0 || headerEnd > pe.size()) {
		failReason = QStringLiteral("bad PE header offset");
		return imports;
	}

	DWORD ntSignature = 0;
	memcpy(&ntSignature, pe.constData() + peOff, sizeof(ntSignature));
	if (ntSignature != 0x00004550) {	// 'PE\0\0'
		failReason = QStringLiteral("bad PE signature");
		return imports;
	}

	IMAGE_FILE_HEADER fh;
	memcpy(&fh, pe.constData() + peOff + 4, sizeof(fh));
	if (fh.NumberOfSections == 0 || fh.NumberOfSections > 96) {
		failReason = QStringLiteral("invalid section count %1").arg(fh.NumberOfSections);
		return imports;
	}

	const qint64 optOff = peOff + 4 + sizeof(IMAGE_FILE_HEADER);
	quint16 magic = 0;
	memcpy(&magic, pe.constData() + optOff, sizeof(magic));

	quint32 importRva = 0;
	const IMAGE_SECTION_HEADER* sections = nullptr;

	if (magic == IMAGE_NT_OPTIONAL_HDR_MAGIC) {		// PE32+（x64 原生）
		IMAGE_OPTIONAL_HEADER64 oh;
		if (optOff + qint64(sizeof(oh)) > pe.size()) {
			failReason = QStringLiteral("bad optional header (PE32+)");
			return imports;
		}
		memcpy(&oh, pe.constData() + optOff, sizeof(oh));
		importRva = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
		sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
			pe.constData() + optOff + sizeof(oh));
	} else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {	// PE32（兼容 32 位 exe）
		IMAGE_OPTIONAL_HEADER32 oh;
		if (optOff + qint64(sizeof(oh)) > pe.size()) {
			failReason = QStringLiteral("bad optional header (PE32)");
			return imports;
		}
		memcpy(&oh, pe.constData() + optOff, sizeof(oh));
		importRva = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
		sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
			pe.constData() + optOff + sizeof(oh));
	} else {
		failReason = QStringLiteral("unknown optional header magic 0x%1")
						 .arg(magic, 4, 16, QChar('0'));
		return imports;
	}

	// section 表边界保护（防畸形 PE）
	if (reinterpret_cast<const char*>(sections)
			+ size_t(fh.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER)
		> pe.constData() + pe.size()) {
		failReason = QStringLiteral("section table out of bounds");
		return imports;
	}

	if (importRva == 0) {	// 无导入表（理论上不会出现在 Qt 程序）
		ok = true;
		return imports;
	}

	quint32 descOff = rvaToOffset(sections, fh.NumberOfSections, importRva);
	if (descOff == 0) {
		failReason = QStringLiteral("import directory rva not mapped");
		return imports;
	}

	// IMAGE_IMPORT_DESCRIPTOR 数组，全零项终止；4096 上限防畸形文件死循环
	for (int i = 0; i < 4096; ++i) {
		if (descOff + sizeof(IMAGE_IMPORT_DESCRIPTOR) > quint32(pe.size()))
			break;
		IMAGE_IMPORT_DESCRIPTOR desc;
		memcpy(&desc, pe.constData() + descOff, sizeof(desc));
		if (desc.Name == 0 && desc.FirstThunk == 0)
			break;
		const QString dll = readCString(
			pe, rvaToOffset(sections, fh.NumberOfSections, desc.Name), 260);
		if (!dll.isEmpty())
			imports.append(dll);
		descOff += sizeof(IMAGE_IMPORT_DESCRIPTOR);
	}

	ok = true;
	return imports;
}

DependencyAnalysis PeDependencyAnalyzer::analyze(const QString& exePath)
{
	DependencyAnalysis result;
	result.exePath = exePath;

	bool readOk = false;
	QString failReason;
	const QStringList imports = readImports(exePath, readOk, failReason);
	if (!readOk) {
		// PRD 16 §8：分析失败仅输出失败原因，不影响主程序
		result.failReason = QStringLiteral("DependencyAnalyzer failed: %1").arg(failReason);
		return result;
	}

	// 进程已加载模块（运行时状态判定基准）
	QHash<QString, QString> loadedByName;	// key = 小写 dll 名
	const QVector<ModuleInfo> loadedModules = ModuleEnumerator::enumerate();
	for (const ModuleInfo& m : loadedModules)
		loadedByName.insert(m.name.toLower(), m.fullPath);

	// 磁盘搜索顺序（PRD 16 §6）：应用目录 → Qt Runtime → PATH
	const QString appDir = QFileInfo(exePath).absolutePath();
	QStringList searchDirs;
	searchDirs.append(appDir);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	const QString qtBin = QLibraryInfo::path(QLibraryInfo::BinariesPath);
#else
	const QString qtBin = QLibraryInfo::location(QLibraryInfo::BinariesPath);
#endif
	if (!qtBin.isEmpty())
		searchDirs.append(qtBin);
	const QStringList pathDirs =
		QString::fromLocal8Bit(qgetenv("PATH")).split(';', Qt::SkipEmptyParts);
	searchDirs.append(pathDirs);

	for (const QString& name : imports) {
		DependencyInfo info;
		info.name = name;
		const QString lower = name.toLower();

		// API Set 虚拟 DLL 由加载器转发（api-ms-win-* → ucrtbase 等），不做缺失误报
		if (lower.startsWith(QStringLiteral("api-ms-"))
			|| lower.startsWith(QStringLiteral("ext-ms-"))) {
			info.status = DependencyStatus::Loaded;
			info.hint = QStringLiteral("System API Set (forwarded by loader)");
			result.dependencies.append(info);
			continue;
		}

		const auto it = loadedByName.constFind(lower);
		if (it != loadedByName.constEnd()) {
			info.status = DependencyStatus::Loaded;
			info.resolvedPath = it.value();
			// Qt DLL 路径异常判定：既不在应用目录也不在 Qt 运行时目录
			if (lower.startsWith(QStringLiteral("qt5"))) {
				// 统一分隔符：模块路径来自 Windows API（反斜杠），
				// appDir/qtBin 来自 Qt API（正斜杠），先归一再比较
				const QString loadedPath = QDir::fromNativeSeparators(it.value());
				const bool fromAppDir =
					loadedPath.startsWith(appDir, Qt::CaseInsensitive);
				const bool fromQtBin =
					!qtBin.isEmpty()
					&& loadedPath.startsWith(qtBin, Qt::CaseInsensitive);
				if (!fromAppDir && !fromQtBin) {
					info.status = DependencyStatus::PathUnexpected;
					info.hint = QStringLiteral(
						"loaded outside app dir and Qt runtime dir, "
						"check PATH or qt.conf");
				}
			}
			result.dependencies.append(info);
			continue;
		}

		// 未加载 → 磁盘 resolve
		QString found;
		for (const QString& dir : searchDirs) {
			const QString candidate = dir + QLatin1Char('/') + name;
			if (QFileInfo::exists(candidate)) {
				found = QDir(dir).absoluteFilePath(name);
				break;
			}
		}

		if (found.isEmpty()) {
			info.status = DependencyStatus::Missing;
			info.hint = QStringLiteral(
				"not found: not deployed (windeployqt), architecture or version "
				"mismatch; searched app dir / Qt runtime / PATH");
		} else {
			// 磁盘找到但未加载：实际解析路径与磁盘搜索结果不一致
			info.status = DependencyStatus::PathUnexpected;
			info.resolvedPath = found;
			info.hint = QStringLiteral(
				"not loaded in process; found on disk at %1, actual resolve "
				"path may differ (PATH / KnownDLLs)").arg(found);
		}
		result.dependencies.append(info);
	}

	// Qt 版本不匹配（模块级）：运行时 qVersion 与编译时 QT_VERSION_STR 不一致
	if (loadedByName.contains(QStringLiteral("qt5core.dll"))
		&& QString::fromLatin1(qVersion()) != QStringLiteral(QT_VERSION_STR)) {
		for (DependencyInfo& d : result.dependencies) {
			if (d.name.compare(QStringLiteral("Qt5Core.dll"), Qt::CaseInsensitive) == 0) {
				d.status = DependencyStatus::VersionMismatch;
				d.hint = QStringLiteral("runtime Qt %1 != built-in Qt %2")
							 .arg(QString::fromLatin1(qVersion()),
								  QStringLiteral(QT_VERSION_STR));
			}
		}
	}

	result.ok = true;
	return result;
}

} // namespace qt_event_watcher

#else // 非 Windows：PRD 16 §9 平台矩阵，SO/ELF 分析后续实现

namespace qt_event_watcher
{

QStringList PeDependencyAnalyzer::readImports(const QString&, bool& ok, QString& failReason)
{
	ok = false;
	failReason = QStringLiteral("unsupported platform");
	return {};
}

DependencyAnalysis PeDependencyAnalyzer::analyze(const QString& exePath)
{
	DependencyAnalysis result;
	result.exePath = exePath;
	result.failReason = QStringLiteral("DependencyAnalyzer failed: unsupported platform");
	return result;
}

} // namespace qt_event_watcher

#endif // Q_OS_WIN
