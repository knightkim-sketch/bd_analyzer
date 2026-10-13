// Help > About bd-analyzer: which build this is.
#pragma once

#include <QString>

class QWidget;

namespace bda::integration
{

//!< The version from the repository's VERSION file at build time, or "unknown".
QString bdaVersion();
//!< The short git hash the build was made from, or "unknown".
QString bdaGitHash();

//!< The About box text: version, git hash and build date.
QString bdaAboutText();
void    showBdaAbout(QWidget *parent);

} // namespace bda::integration
