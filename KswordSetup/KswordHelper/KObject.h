#ifndef KSWORD_HELPER_KOBJECT_HEAD_FILE
#define KSWORD_HELPER_KOBJECT_HEAD_FILE

#include "KVariant.h"

#include <map>
#include <string>
#include <vector>

// KObject is a Qt-like base object without Qt dependency. Parent objects own
// their child KObject pointers, so children attached to a parent should be heap
// allocated unless they are detached before destruction.
class KObject {
public:
    // Constructors create an object with an optional parent and object name.
    explicit KObject(KObject* parent = nullptr);
    KObject(const std::string& objectName, KObject* parent = nullptr);

    // The virtual destructor detaches from the parent and deletes all children.
    virtual ~KObject();

    // Copying is disabled because parent/child ownership is pointer based.
    KObject(const KObject&) = delete;
    KObject& operator=(const KObject&) = delete;

    // objectName accessors read or replace the diagnostic object name.
    std::string objectName() const;
    void setObjectName(const std::string& objectName);

    // parent returns the current parent pointer or nullptr for root objects.
    KObject* parent() const;

    // setParent reparents this object. It returns false when the requested
    // parent would create a cycle; otherwise it updates both child lists.
    bool setParent(KObject* parent);

    // children returns a snapshot of direct child pointers.
    std::vector<KObject*> children() const;

    // childCount returns the current direct child count.
    std::size_t childCount() const;

    // findChild searches by objectName. Recursive search includes descendants.
    KObject* findChild(const std::string& objectName, bool recursive = true) const;

    // deleteChildren deletes all direct children and clears the child list.
    void deleteChildren();

    // Dynamic property APIs store small helper values by string key.
    void setProperty(const std::string& name, const KVariant& value);
    KVariant property(const std::string& name, const KVariant& defaultValue = KVariant()) const;
    bool hasProperty(const std::string& name) const;
    bool removeProperty(const std::string& name);
    std::vector<std::string> dynamicPropertyNames() const;

private:
    // Internal helpers mutate child lists without changing the child's parent.
    void appendChildPointer(KObject* child);
    void removeChildPointer(KObject* child);

    // hasAncestor returns true when possibleAncestor is in this object's parent chain.
    bool hasAncestor(const KObject* possibleAncestor) const;

    // m_objectName is an optional diagnostic/business name.
    std::string m_objectName;

    // m_parent is not owned by this object; parent owns this object by convention.
    KObject* m_parent;

    // m_children stores owned direct children.
    std::vector<KObject*> m_children;

    // m_properties stores dynamic values similar to QObject dynamic properties.
    std::map<std::string, KVariant> m_properties;
};

#endif
