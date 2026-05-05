#pragma once

#include <QString>
#include <QJsonObject>
#include <QSettings>

class NvApp
{
public:
    NvApp() = default;

    NvApp(int id, const QString& name, bool hdrSupported = false)
        : m_Id(id), m_Name(name), m_HdrSupported(hdrSupported) {}

    explicit NvApp(QSettings& settings)
    {
        m_Id = settings.value("id").toInt();
        m_Name = settings.value("name").toString();
        m_HdrSupported = settings.value("hdr").toBool();
    }

    bool isInitialized() const { return m_Id != 0 && !m_Name.isEmpty(); }

    int id() const { return m_Id; }
    QString name() const { return m_Name; }
    bool hdrSupported() const { return m_HdrSupported; }

    QJsonObject toJson() const
    {
        QJsonObject obj;
        obj["id"] = m_Id;
        obj["name"] = m_Name;
        obj["hdrSupported"] = m_HdrSupported;
        return obj;
    }

    void serialize(QSettings& settings) const
    {
        settings.setValue("id", m_Id);
        settings.setValue("name", m_Name);
        settings.setValue("hdr", m_HdrSupported);
    }

    bool operator==(const NvApp& other) const
    {
        return m_Id == other.m_Id && m_Name == other.m_Name
            && m_HdrSupported == other.m_HdrSupported;
    }

    bool operator!=(const NvApp& other) const { return !(*this == other); }

private:
    int m_Id = 0;
    QString m_Name;
    bool m_HdrSupported = false;
};
