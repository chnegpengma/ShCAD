
#include "ShEdaLoader.h"
#include <qfile.h>
#include <qfileinfo.h>
#include <qdir.h>
#include <qtextstream.h>
#include <qdebug.h>
#include <qmessagebox.h>

#include "Interface\ShCADWidget.h"
#include "Base\ShLayer.h"
#include "Base\ShLayerTable.h"
#include "Entity\Leaf\ShLine.h"
#include "Entity\Leaf\ShDot.h"
#include "Entity\Composite\ShEntityTable.h"
#include "Data\ShPropertyData.h"
#include "Data\ShColor.h"
#include "Data\ShLineStyle.h"
#include "Entity\Private\ShEntityData.h"
#include "Base\ShGlobal.h"
#include "Base\ShVariable.h"

ShEdaLoader::ShEdaLoader()
	: lastFormat(FormatUnknown) {

}

ShEdaLoader::~ShEdaLoader() {

}

QString ShEdaLoader::formatToString(Format format) {

	switch (format) {
	case FormatBrd:   return "Allegro Board (.brd)";
	case FormatOdbPP: return "ODB++ Job";
	case FormatSpd:   return "SPD File (.spd)";
	default:          return "Unknown";
	}
}

ShEdaLoader::Format ShEdaLoader::detectFormat(const QString &filePath) {

	QFileInfo info(filePath);

	// ODB++ jobs are delivered as a directory tree whose root (or whose
	// "ODB" sub-folder) contains the pro and misc files plus the steps /
	// layers hierarchy.
	if (info.isDir()) {

		QString parentDir = info.absoluteFilePath();
		QStringList candidates;
		candidates << parentDir;
		candidates << QDir(parentDir).absoluteFilePath("ODB");

		for (const QString & candidate : candidates) {

			QDir dir(candidate);
			if (dir.exists("pro") || dir.exists("misc") ||
				dir.exists("steps") || dir.exists("layers")) {

				return FormatOdbPP;
			}
		}

		return FormatUnknown;
	}

	QString suffix = info.suffix().toLower();

	if (suffix == "brd")
		return FormatBrd;

	if (suffix == "spd")
		return FormatSpd;

	// Fall back to content sniffing for extension-less files.
	QFile file(filePath);
	if (file.open(QIODevice::ReadOnly)) {

		QByteArray head = file.read(512);
		file.close();

		if (head.contains("Allegro") || head.contains("axl"))
			return FormatBrd;

		if (head.contains("ODB"))
			return FormatOdbPP;

		if (head.contains("<SPD") || head.contains("SPD_FILE"))
			return FormatSpd;
	}

	return FormatUnknown;
}

bool ShEdaLoader::load(const QString &filePath, ShCADWidget *widget) {

	this->lastFilePath = filePath;
	this->lastFormat = FormatUnknown;
	this->lastSummary.clear();

	if (widget == nullptr) {
		qDebug() << "ShEdaLoader::load  -- no active CAD widget";
		return false;
	}

	Format format = this->detectFormat(filePath);
	this->lastFormat = format;

	bool ok = false;

	switch (format) {
	case FormatBrd:   ok = this->loadBrd(filePath, widget);   break;
	case FormatOdbPP: ok = this->loadOdbPP(filePath, widget); break;
	case FormatSpd:   ok = this->loadSpd(filePath, widget);    break;
	default:
		this->lastSummary = "Unsupported EDA file: " + QFileInfo(filePath).fileName();
		return false;
	}

	return ok;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// .brd loader
//
// Cadence Allegro .brd files are binary, however some other tools (Eagle, KiCad
// via plug-ins, ...) emit text/XML based .brd files. This loader detects both
// flavours and extracts whatever geometric data it can understand, falling
// back to a header-only inspection when only the binary form is available.
/////////////////////////////////////////////////////////////////////////////////////////////
bool ShEdaLoader::loadBrd(const QString &filePath, ShCADWidget *widget) {

	QFile file(filePath);
	if (!file.open(QIODevice::ReadOnly)) {

		this->lastSummary = "Failed to open .brd file: " + filePath;
		return false;
	}

	QString formatTag;
	qint64 fileSize = file.size();
	QString extraInfo;

	// Try to detect text/XML based .brd first.
	QByteArray header = file.read(8192);
	file.seek(0);

	bool isText = false;
	for (char c : header.left(64)) {
		if (c == 0) { isText = false; break; }
		if (c == '<' || c == '#') { isText = true; break; }
	}

	int entityCount = 0;
	QString boardName = QFileInfo(filePath).completeBaseName();

	if (isText && (header.contains("<?xml") || header.contains("<board"))) {

		formatTag = "Eagle / XML .brd";

		QString content = QString::fromUtf8(header);

		QRegExp wireRx("<wire\\s+x1=\"([\\-\\d.]+)\"\\s+y1=\"([\\-\\d.]+)\"\\s+x2=\"([\\-\\d.]+)\"\\s+y2=\"([\\-\\d.]+)\"");
		QRegExp viaRx("<via\\s+x=\"([\\-\\d.]+)\"\\s+y=\"([\\-\\d.]+)\"");

		int pos = 0;
		ShLayer *layer = widget->getCurrentLayer();
		ShPropertyData prop = widget->getPropertyData();

		while ((pos = wireRx.indexIn(content, pos)) != -1) {

			double x1 = wireRx.cap(1).toDouble();
			double y1 = wireRx.cap(2).toDouble();
			double x2 = wireRx.cap(3).toDouble();
			double y2 = wireRx.cap(4).toDouble();

			ShLine *line = new ShLine(prop, ShLineData(ShPoint3d(x1, y1), ShPoint3d(x2, y2)), layer);
			widget->getEntityTable().add(line);
			++entityCount;
			pos += wireRx.matchedLength();
		}

		pos = 0;
		while ((pos = viaRx.indexIn(content, pos)) != -1) {

			double x = viaRx.cap(1).toDouble();
			double y = viaRx.cap(2).toDouble();

			ShDot *dot = new ShDot(ShPoint3d(x, y), prop, layer);
			widget->getEntityTable().add(dot);
			++entityCount;
			pos += viaRx.matchedLength();
		}

		extraInfo = QString("Extracted %1 wire(s) / via(s) from XML board.").arg(entityCount);
	}
	else {

		// Binary Allegro file -- we cannot fully decode it without a license,
		// so we report the magic header bytes and the file size.
		formatTag = "Allegro binary .brd";

		QString magic = QString::fromLatin1(header.left(16).toHex());
		extraInfo = QString("Magic: %1  Size: %2 bytes").arg(magic).arg(fileSize);

		entityCount = 0;
	}

	file.close();

	this->lastSummary = this->buildSummary(FormatBrd, filePath, entityCount,
		QString("Format: %1\nBoard: %2\n%3").arg(formatTag).arg(boardName).arg(extraInfo));

	widget->update((DrawType)(DrawType::DrawCaptureImage | DrawType::DrawAddedEntities));
	widget->captureImage();

	return true;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// ODB++ loader
//
// ODB++ jobs are organised as a directory tree:
//
//   <job>/
//     pro
//     misc/
//     steps/
//       <step>/
//         stephdr
//         layers/
//           <layer>/
//             ...
//
// The board outline is stored inside the "profile" file of every step. The
// file is text based with records such as:
//   #Header
//   L  T  10 20 100 20 100 80 10 80 10 20    (Line segments, X/Y pairs)
//   A  R  50 50 30 0 270                     (Arc)
//   #End
//
// We parse those records and turn them into ShLine entities.
/////////////////////////////////////////////////////////////////////////////////////////////
bool ShEdaLoader::loadOdbPP(const QString &filePath, ShCADWidget *widget) {

	QFileInfo info(filePath);
	QString jobRoot = info.absoluteFilePath();

	if (info.isFile())
		jobRoot = info.absolutePath();

	// Walk up one level if the supplied path is the misc/ or steps/ folder.
	if (QFileInfo(jobRoot).fileName() == "steps" ||
		QFileInfo(jobRoot).fileName() == "misc")
		jobRoot = QFileInfo(jobRoot).absolutePath();

	// Find the actual steps folder, it might live directly under the job
	// root or inside an "ODB" sub-folder.
	QString stepsDir;
	QStringList candidates;
	candidates << QDir(jobRoot).absoluteFilePath("steps");
	candidates << QDir(jobRoot).absoluteFilePath("ODB/steps");

	for (const QString & candidate : candidates) {

		if (QFileInfo(candidate).isDir()) {
			stepsDir = candidate;
			break;
		}
	}

	QStringList stepList;
	QStringList layerList;
	int entityCount = 0;
	QString extraInfo;

	ShLayer *layer = widget->getCurrentLayer();
	ShPropertyData prop = widget->getPropertyData();

	if (!stepsDir.isEmpty()) {

		stepList = QDir(stepsDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);

		for (const QString & stepName : stepList) {

			QString stepPath = QDir(stepsDir).absoluteFilePath(stepName);
			QString layersPath = QDir(stepPath).absoluteFilePath("layers");

			QStringList stepLayers = QDir(layersPath).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
			layerList.append(stepLayers);

			QString profilePath = QDir(stepPath).absoluteFilePath("profile");

			if (!QFileInfo(profilePath).exists())
				continue;

			QFile pf(profilePath);
			if (!pf.open(QIODevice::ReadOnly | QIODevice::Text))
				continue;

			QStringList lines = QString::fromUtf8(pf.readAll()).split('\n');
			pf.close();

			for (const QString & raw : lines) {

				QString line = raw.trimmed();
				if (line.isEmpty() || line.startsWith("#"))
					continue;

				QStringList tokens = line.split(' ', QString::SkipEmptyParts);
				if (tokens.size() < 1)
					continue;

				QString code = tokens.at(0).toUpper();

				// Profile line record: L <symbol> <x1> <y1> <x2> <y2> ...
				// The first non-coordinate column is the line symbol.
				if (code == "L" && tokens.size() >= 7) {

					int idx = 1;
					if (tokens.size() % 2 == 0) // skip the symbol column
						++idx;

					while (idx + 3 < tokens.size()) {

						double x1 = tokens.at(idx).toDouble();
						double y1 = tokens.at(idx + 1).toDouble();
						double x2 = tokens.at(idx + 2).toDouble();
						double y2 = tokens.at(idx + 3).toDouble();

						ShLine *seg = new ShLine(prop,
							ShLineData(ShPoint3d(x1, y1), ShPoint3d(x2, y2)), layer);
						widget->getEntityTable().add(seg);
						++entityCount;
						idx += 2;
					}
				}
			}
		}
	}

	QString jobName = QFileInfo(jobRoot).fileName();

	QStringList uniqueLayers = layerList;
	uniqueLayers.removeDuplicates();

	extraInfo = QString("Job: %1\nSteps: %2\nLayers: %3")
		.arg(jobName)
		.arg(stepList.size())
		.arg(uniqueLayers.size());

	this->lastSummary = this->buildSummary(FormatOdbPP, filePath, entityCount, extraInfo);

	widget->update((DrawType)(DrawType::DrawCaptureImage | DrawType::DrawAddedEntities));
	widget->captureImage();

	return true;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// .spd loader
//
// SPD files used by some Chinese PCB layout tools are XML documents whose root
// tag is <SPD> (or similar). For non XML variants we scan for ASCII coordinate
// records that look like "L x1 y1 x2 y2".
/////////////////////////////////////////////////////////////////////////////////////////////
bool ShEdaLoader::loadSpd(const QString &filePath, ShCADWidget *widget) {

	QFile file(filePath);
	if (!file.open(QIODevice::ReadOnly)) {

		this->lastSummary = "Failed to open .spd file: " + filePath;
		return false;
	}

	qint64 fileSize = file.size();
	QString content = QString::fromUtf8(file.readAll());
	file.close();

	QString formatTag;
	QString extraInfo;
	int entityCount = 0;

	ShLayer *layer = widget->getCurrentLayer();
	ShPropertyData prop = widget->getPropertyData();

	if (content.contains("<?xml") || content.contains("<SPD")) {

		formatTag = "XML .spd";

		QRegExp lineRx("<LINE[^>]*x1\\s*=\\s*\"([\\-\\d.]+)\"[^>]*y1\\s*=\\s*\"([\\-\\d.]+)\"[^>]*x2\\s*=\\s*\"([\\-\\d.]+)\"[^>]*y2\\s*=\\s*\"([\\-\\d.]+)\"");
		QRegExp padRx("<PAD[^>]*x\\s*=\\s*\"([\\-\\d.]+)\"[^>]*y\\s*=\\s*\"([\\-\\d.]+)\"");

		int pos = 0;
		while ((pos = lineRx.indexIn(content, pos)) != -1) {

			double x1 = lineRx.cap(1).toDouble();
			double y1 = lineRx.cap(2).toDouble();
			double x2 = lineRx.cap(3).toDouble();
			double y2 = lineRx.cap(4).toDouble();

			ShLine *line = new ShLine(prop, ShLineData(ShPoint3d(x1, y1), ShPoint3d(x2, y2)), layer);
			widget->getEntityTable().add(line);
			++entityCount;
			pos += lineRx.matchedLength();
		}

		pos = 0;
		while ((pos = padRx.indexIn(content, pos)) != -1) {

			double x = padRx.cap(1).toDouble();
			double y = padRx.cap(2).toDouble();

			ShDot *dot = new ShDot(ShPoint3d(x, y), prop, layer);
			widget->getEntityTable().add(dot);
			++entityCount;
			pos += padRx.matchedLength();
		}

		extraInfo = QString("Extracted %1 line/pad primitives.").arg(entityCount);
	}
	else {

		// Plain text SPD: scan for "L x1 y1 x2 y2" style records.
		formatTag = "Text .spd";

		QStringList lines = content.split('\n');
		int parsed = 0;
		for (const QString & raw : lines) {

			QString line = raw.trimmed();
			if (line.isEmpty() || line.startsWith("#") || line.startsWith(";"))
				continue;

			QStringList tokens = line.split(QRegExp("\\s+"), QString::SkipEmptyParts);
			if (tokens.size() < 5)
				continue;

			QString code = tokens.at(0).toUpper();
			if (code != "L" && code != "LINE")
				continue;

			double x1 = tokens.at(1).toDouble();
			double y1 = tokens.at(2).toDouble();
			double x2 = tokens.at(3).toDouble();
			double y2 = tokens.at(4).toDouble();

			ShLine *seg = new ShLine(prop,
				ShLineData(ShPoint3d(x1, y1), ShPoint3d(x2, y2)), layer);
			widget->getEntityTable().add(seg);
			++entityCount;
			++parsed;
		}

		extraInfo = QString("Parsed %1 line records.").arg(parsed);
	}

	QString boardName = QFileInfo(filePath).completeBaseName();
	this->lastSummary = this->buildSummary(FormatSpd, filePath, entityCount,
		QString("Format: %1\nBoard: %2\n%3").arg(formatTag).arg(boardName).arg(extraInfo));

	widget->update((DrawType)(DrawType::DrawCaptureImage | DrawType::DrawAddedEntities));
	widget->captureImage();

	return true;
}

QString ShEdaLoader::buildSummary(Format format, const QString &filePath,
	int entityCount, const QString &extraInfo) const {

	QString summary;
	summary += "EDA file loaded.\n";
	summary += "File:   " + QFileInfo(filePath).fileName() + "\n";
	summary += "Format: " + formatToString(format) + "\n";
	summary += QString("Entities added: %1\n").arg(entityCount);

	if (!extraInfo.isEmpty())
		summary += extraInfo + "\n";

	return summary;
}
