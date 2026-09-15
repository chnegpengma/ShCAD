
#ifndef _SHEDALOADER_H
#define _SHEDALOADER_H

#include <qstring.h>
#include <qlist.h>
#include "Base\ShSingleton.h"

class ShEntity;
class ShCADWidget;

// EDA file format loader.
// Supports Cadence Allegro .brd, ODB++ directories, and .spd files.
// The parser implementations are intentionally lightweight: they detect the
// format, extract the available structural information (board outline, layer
// names, component count, ...), and feed that information back to the caller
// so the CAD widget can render a basic preview.
class ShEdaLoader {

	DeclareSingleton(ShEdaLoader)

public:
	enum Format {
		FormatUnknown,
		FormatBrd,    // Cadence Allegro board file
		FormatOdbPP,  // ODB++ job directory
		FormatSpd,    // Sprint-PCB / SPD file
	};

private:
	QString lastFilePath;
	Format lastFormat;
	QString lastSummary;

public:
	// Detect the EDA format of the supplied path. For ODB++ the path may
	// either point at the job root directory or at the pro/features file
	// inside the job.
	Format detectFormat(const QString &filePath);

	// Load the file into the supplied CAD widget. Returns true on success.
	// On failure, getLastError() returns a human readable description.
	bool load(const QString &filePath, ShCADWidget *widget);

	inline const QString& getLastFilePath() const { return this->lastFilePath; }
	inline Format getLastFormat() const { return this->lastFormat; }
	inline const QString& getLastSummary() const { return this->lastSummary; }

	static QString formatToString(Format format);

private:
	bool loadBrd(const QString &filePath, ShCADWidget *widget);
	bool loadOdbPP(const QString &filePath, ShCADWidget *widget);
	bool loadSpd(const QString &filePath, ShCADWidget *widget);

	// Build a short textual summary that describes what has been loaded so it
	// can be reported back to the user (e.g. via a message box).
	QString buildSummary(Format format, const QString &filePath,
		int entityCount, const QString &extraInfo) const;
};

#endif //_SHEDALOADER_H
