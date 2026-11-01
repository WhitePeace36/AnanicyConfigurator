#pragma once

#include <QLabel>
#include <QResizeEvent>

// Single-line label that never wraps or grows: text that does not fit is
// shortened with "…" and the full text is shown as tooltip.
// Uses the theme's placeholder-text colour so it is readable on dark themes.
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(Qt::TextElideMode mode = Qt::ElideRight, QWidget *parent = nullptr)
        : QLabel(parent), m_mode(mode)
    {
        setWordWrap(false);
        setForegroundRole(QPalette::PlaceholderText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setMinimumHeight(fontMetrics().height());
    }

    void setFullText(const QString &text)
    {
        m_full = text;
        setToolTip(text);
        updateElided();
    }
    QString fullText() const { return m_full; }

    QSize sizeHint() const override { return {fontMetrics().horizontalAdvance(m_full), fontMetrics().height()}; }
    QSize minimumSizeHint() const override { return {0, fontMetrics().height()}; }

protected:
    void resizeEvent(QResizeEvent *ev) override
    {
        QLabel::resizeEvent(ev);
        updateElided();
    }

private:
    void updateElided() { QLabel::setText(fontMetrics().elidedText(m_full, m_mode, qMax(0, width()))); }

    Qt::TextElideMode m_mode;
    QString m_full;
};
