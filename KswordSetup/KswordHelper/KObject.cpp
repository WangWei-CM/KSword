#include "KObject.h"

#include <algorithm>

KObject::KObject(KObject* parent)
    : m_objectName(), m_parent(nullptr), m_children(), m_properties() {
    setParent(parent);
}

KObject::KObject(const std::string& objectName, KObject* parent)
    : m_objectName(objectName), m_parent(nullptr), m_children(), m_properties() {
    setParent(parent);
}

KObject::~KObject() {
    if (m_parent) {
        m_parent->removeChildPointer(this);
        m_parent = nullptr;
    }
    deleteChildren();
}

std::string KObject::objectName() const {
    return m_objectName;
}

void KObject::setObjectName(const std::string& objectName) {
    m_objectName = objectName;
}

KObject* KObject::parent() const {
    return m_parent;
}

bool KObject::setParent(KObject* parent) {
    if (parent == m_parent) {
        return true;
    }
    if (parent == this) {
        return false;
    }
    if (parent && parent->hasAncestor(this)) {
        return false;
    }

    if (m_parent) {
        m_parent->removeChildPointer(this);
    }
    m_parent = parent;
    if (m_parent) {
        m_parent->appendChildPointer(this);
    }
    return true;
}

std::vector<KObject*> KObject::children() const {
    return m_children;
}

std::size_t KObject::childCount() const {
    return m_children.size();
}

KObject* KObject::findChild(const std::string& objectName, bool recursive) const {
    for (std::vector<KObject*>::const_iterator it = m_children.begin(); it != m_children.end(); ++it) {
        KObject* child = *it;
        if (!child) {
            continue;
        }
        if (child->objectName() == objectName) {
            return child;
        }
        if (recursive) {
            KObject* nested = child->findChild(objectName, true);
            if (nested) {
                return nested;
            }
        }
    }
    return nullptr;
}

void KObject::deleteChildren() {
    std::vector<KObject*> ownedChildren = m_children;
    m_children.clear();

    for (std::vector<KObject*>::iterator it = ownedChildren.begin(); it != ownedChildren.end(); ++it) {
        KObject* child = *it;
        if (!child) {
            continue;
        }
        child->m_parent = nullptr;
        delete child;
    }
}

void KObject::setProperty(const std::string& name, const KVariant& value) {
    m_properties[name] = value;
}

KVariant KObject::property(const std::string& name, const KVariant& defaultValue) const {
    std::map<std::string, KVariant>::const_iterator it = m_properties.find(name);
    if (it == m_properties.end()) {
        return defaultValue;
    }
    return it->second;
}

bool KObject::hasProperty(const std::string& name) const {
    return m_properties.find(name) != m_properties.end();
}

bool KObject::removeProperty(const std::string& name) {
    return m_properties.erase(name) > 0;
}

std::vector<std::string> KObject::dynamicPropertyNames() const {
    std::vector<std::string> result;
    result.reserve(m_properties.size());
    for (std::map<std::string, KVariant>::const_iterator it = m_properties.begin(); it != m_properties.end(); ++it) {
        result.push_back(it->first);
    }
    return result;
}

void KObject::appendChildPointer(KObject* child) {
    if (!child) {
        return;
    }
    if (std::find(m_children.begin(), m_children.end(), child) == m_children.end()) {
        m_children.push_back(child);
    }
}

void KObject::removeChildPointer(KObject* child) {
    m_children.erase(std::remove(m_children.begin(), m_children.end(), child), m_children.end());
}

bool KObject::hasAncestor(const KObject* possibleAncestor) const {
    const KObject* current = m_parent;
    while (current) {
        if (current == possibleAncestor) {
            return true;
        }
        current = current->m_parent;
    }
    return false;
}
