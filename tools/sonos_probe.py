#!/usr/bin/env python3
"""Read-only local Sonos discovery, topology, and favorites probe.

This deliberately implements only UPnP reads. It never calls transport,
rendering-control, queue mutation, or group mutation actions.
"""

from __future__ import print_function

import argparse
import json
import os
import re
import socket
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET


SSDP_ADDRESS = ("239.255.255.250", 1900)
SSDP_TARGET = "urn:schemas-upnp-org:device:ZonePlayer:1"
SOAP_ENV = "http://schemas.xmlsoap.org/soap/envelope/"
CONTENT_DIRECTORY = "urn:schemas-upnp-org:service:ContentDirectory:1"
ZONE_GROUP_TOPOLOGY = "urn:schemas-upnp-org:service:ZoneGroupTopology:1"
MUSIC_SERVICES = "urn:schemas-upnp-org:service:MusicServices:1"
DIDL = "urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/"
DC = "http://purl.org/dc/elements/1.1/"
UPNP = "urn:schemas-upnp-org:metadata-1-0/upnp/"
ALLOWED_SERVICE_NAMES = ("apple music", "sonos radio")
DEFAULT_TIMEOUT = 3.0
PAGE_SIZE = 100
MAX_PAGES = 1000


class ProbeError(Exception):
    """A network or protocol failure that should be reported to the user."""


def local_name(tag):
    return tag.rsplit("}", 1)[-1] if "}" in tag else tag


def child_text(element, name, default=""):
    for child in element.iter():
        if child is not element and local_name(child.tag) == name and child.text:
            return child.text.strip()
    return default


def parse_xml(data, what):
    try:
        return ET.fromstring(data)
    except (ET.ParseError, TypeError, ValueError) as exc:
        raise ProbeError("Malformed {} XML: {}".format(what, exc))


def element_text(element):
    return "" if element is None or element.text is None else element.text.strip()


def soap_envelope(action, arguments=None):
    """Build a SOAP 1.1 envelope with escaped argument values."""
    body = ET.Element("{%s}Envelope" % SOAP_ENV)
    body.set("{%s}encodingStyle" % SOAP_ENV,
             "http://schemas.xmlsoap.org/soap/encoding/")
    body_node = ET.SubElement(body, "{%s}Body" % SOAP_ENV)
    action_node = ET.SubElement(body_node, action)
    for name, value in (arguments or {}).items():
        node = ET.SubElement(action_node, name)
        node.text = str(value)
    return ET.tostring(body, encoding="utf-8", xml_declaration=True)


def http_request(url, data=None, headers=None, timeout=DEFAULT_TIMEOUT):
    req = urllib.request.Request(url, data=data, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return response.read()
    except (urllib.error.URLError, OSError, socket.timeout) as exc:
        raise ProbeError("Request to {} failed: {}".format(url, exc))


def soap_call(control_url, service_type, action, arguments=None,
              timeout=DEFAULT_TIMEOUT):
    data = soap_envelope("{%s}%s" % (service_type, action), arguments)
    headers = {
        "Content-Type": 'text/xml; charset="utf-8"',
        "SOAPAction": '"{}#{}"'.format(service_type, action),
    }
    root = parse_xml(http_request(control_url, data, headers, timeout), action)
    fault = next((node for node in root.iter() if local_name(node.tag) == "Fault"), None)
    if fault is not None:
        message = child_text(fault, "faultstring", "UPnP SOAP fault")
        raise ProbeError("{}: {}".format(action, message))
    return root


def discover(timeout=2.5):
    """Return SSDP device-description URLs; multicast errors are non-fatal."""
    message = ("M-SEARCH * HTTP/1.1\r\n"
               "HOST: 239.255.255.250:1900\r\n"
               'MAN: "ssdp:discover"\r\n'
               "MX: 2\r\n"
               "ST: {}\r\n\r\n".format(SSDP_TARGET)).encode("ascii")
    locations = set()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.settimeout(0.2)
    try:
        sock.sendto(message, SSDP_ADDRESS)
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                response, _ = sock.recvfrom(65535)
            except socket.timeout:
                continue
            for line in response.decode("iso-8859-1", "replace").splitlines():
                if line.lower().startswith("location:"):
                    locations.add(line.split(":", 1)[1].strip())
    except OSError:
        pass
    finally:
        sock.close()
    return sorted(locations)


def description_url_for_ip(ip):
    return "http://{}:1400/xml/device_description.xml".format(ip)


def service_map(description_url, root):
    parsed = urllib.parse.urlsplit(description_url)
    base = "{}://{}".format(parsed.scheme, parsed.netloc)
    result = {}
    for service in root.iter():
        if local_name(service.tag) != "service":
            continue
        service_type = child_text(service, "serviceType")
        control = child_text(service, "controlURL")
        if service_type and control:
            result[service_type] = urllib.parse.urljoin(base, control)
    return result


def fetch_device(description_url, timeout=DEFAULT_TIMEOUT):
    root = parse_xml(http_request(description_url, timeout=timeout), "device description")
    device = next((node for node in root.iter() if local_name(node.tag) == "device"), root)
    info = {
        "description_url": description_url,
        "ip": urllib.parse.urlsplit(description_url).hostname,
        "udn": child_text(device, "UDN"),
        "room": child_text(device, "roomName") or child_text(device, "friendlyName"),
        "friendly_name": child_text(device, "friendlyName"),
        "model_name": child_text(device, "modelName"),
        "model_number": child_text(device, "modelNumber"),
        "manufacturer": child_text(device, "manufacturer"),
        "services": service_map(description_url, root),
    }
    if "sonos" not in info["manufacturer"].casefold():
        raise ProbeError("{} is not a Sonos device".format(description_url))
    return info


def _required_service(device, service_type):
    url = device["services"].get(service_type)
    if not url:
        raise ProbeError("{} has no {} service".format(device["ip"], service_type))
    return url


def _first_action_result(root, action):
    for node in root.iter():
        if local_name(node.tag) == action + "Response":
            for child in node:
                if local_name(child.tag) == action + "Result":
                    return child
            # Sonos UPnP actions commonly put their named output arguments
            # directly inside the response element (without *Result wrapper).
            return node
    raise ProbeError("{} response is missing {}Result".format(action, action))


def get_topology(device, timeout=DEFAULT_TIMEOUT):
    root = soap_call(_required_service(device, ZONE_GROUP_TOPOLOGY),
                     ZONE_GROUP_TOPOLOGY, "GetZoneGroupState", timeout=timeout)
    result = _first_action_result(root, "GetZoneGroupState")
    text = child_text(result, "ZoneGroupState")
    if not text:
        # Some firmware returns the state as the result node's direct text.
        text = element_text(result)
    if not text:
        raise ProbeError("GetZoneGroupState returned an empty topology")
    topology = parse_xml(text.encode("utf-8"), "zone topology")
    groups = []
    for group in topology.iter():
        if local_name(group.tag) != "ZoneGroup":
            continue
        rooms = []
        for member in group.iter():
            if local_name(member.tag) != "ZoneGroupMember":
                continue
            rooms.append({
                "uuid": member.attrib.get("UUID", ""),
                "room": member.attrib.get("ZoneName", ""),
                "invisible": member.attrib.get("Invisible", "0") == "1",
                "coordinator": member.attrib.get("UUID", "") == group.attrib.get("Coordinator"),
            })
        groups.append({"coordinator": group.attrib.get("Coordinator", ""), "rooms": rooms})
    return {"household_id": topology.attrib.get("HouseholdID", ""), "groups": groups}


def list_available_services(device, timeout=DEFAULT_TIMEOUT):
    """Return the raw service descriptors used for provider classification."""
    root = soap_call(_required_service(device, MUSIC_SERVICES), MUSIC_SERVICES,
                     "ListAvailableServices", {"Version": "1"}, timeout)
    result = _first_action_result(root, "ListAvailableServices")
    payload = None
    for node in result.iter():
        if local_name(node.tag) == "AvailableServiceDescriptorList":
            payload = node.text
            if payload:
                break
            payload = ET.tostring(node, encoding="unicode")
    if not payload:
        # A few service stacks return descriptor elements directly.
        payload = ET.tostring(result, encoding="unicode")
    descriptors_xml = parse_xml(payload.encode("utf-8"), "available-services")
    descriptors = []
    for node in descriptors_xml.iter():
        if local_name(node.tag) not in ("Service", "ServiceDescriptor", "AvailableServiceDescriptor"):
            continue
        descriptor = dict(node.attrib)
        descriptor.update({local_name(child.tag): element_text(child) for child in node})
        descriptor["attributes"] = dict(node.attrib)
        if descriptor:
            descriptors.append(descriptor)
    if not descriptors:
        raise ProbeError("ListAvailableServices returned no service descriptors")
    return descriptors


def _didl_items(didl_xml):
    root = parse_xml(didl_xml.encode("utf-8"), "favorites DIDL")
    items = []
    for node in root:
        kind = local_name(node.tag)
        if kind not in ("item", "container"):
            continue
        metadata = ET.tostring(node, encoding="unicode")
        item = {
            "kind": kind,
            "id": node.attrib.get("id", ""),
            "parent_id": node.attrib.get("parentID", ""),
            "restricted": node.attrib.get("restricted", ""),
            "title": child_text(node, "title"),
            "creator": child_text(node, "creator"),
            "artist": child_text(node, "artist"),
            "album": child_text(node, "album"),
            "class": child_text(node, "class"),
            "resources": [],
            "service_markers": [],
            "metadata_xml": metadata,
        }
        for child in node:
            name = local_name(child.tag)
            if name == "res":
                item["resources"].append({"uri": element_text(child), "attributes": dict(child.attrib)})
            elif name == "desc":
                item["service_markers"].append({"text": element_text(child), "attributes": dict(child.attrib)})
            elif name == "resMD" and element_text(child):
                # Sonos stores the provider account descriptor as escaped
                # DIDL inside rincon:resMD. Preserve and inspect its <desc>.
                try:
                    embedded = parse_xml(element_text(child).encode("utf-8"), "embedded resource metadata")
                except ProbeError:
                    item["service_markers"].append({"field": "resMD", "text": element_text(child)})
                else:
                    for desc in embedded.iter():
                        if local_name(desc.tag) == "desc":
                            item["service_markers"].append({
                                "field": "resMD:desc",
                                "text": element_text(desc),
                                "attributes": dict(desc.attrib),
                            })
            elif name in ("serviceID", "serviceId", "sid", "provider"):
                item["service_markers"].append({"field": name, "value": element_text(child)})
        items.append(item)
    return items


def browse_favorites(device, page_size=PAGE_SIZE, timeout=DEFAULT_TIMEOUT):
    if page_size < 1:
        raise ValueError("page_size must be positive")
    control = _required_service(device, CONTENT_DIRECTORY)
    start = 0
    total = None
    items = []
    for _ in range(MAX_PAGES):
        root = soap_call(control, CONTENT_DIRECTORY, "Browse", {
            "ObjectID": "FV:2",
            "BrowseFlag": "BrowseDirectChildren",
            "Filter": "*",
            "StartingIndex": start,
            "RequestedCount": page_size,
            "SortCriteria": "",
        }, timeout)
        result = _first_action_result(root, "Browse")
        result_xml = child_text(result, "Result")
        if result_xml is None:
            raise ProbeError("Browse response is missing Result")
        returned = _didl_items(result_xml)
        number_returned = child_text(result, "NumberReturned")
        total_text = child_text(result, "TotalMatches")
        update_id = child_text(result, "UpdateID")
        try:
            reported = int(number_returned) if number_returned else len(returned)
            page_total = int(total_text) if total_text else None
        except ValueError:
            raise ProbeError("Browse returned a non-numeric pagination count")
        if reported != len(returned):
            raise ProbeError("Browse NumberReturned={} but parsed {} entries".format(reported, len(returned)))
        if total is None:
            total = page_total
        elif page_total is not None and total != page_total:
            raise ProbeError("Favorites total changed during pagination ({} to {})".format(total, page_total))
        items.extend(returned)
        if reported == 0:
            break
        start += reported
        if total is not None and start >= total:
            break
        if reported < page_size:
            break
    else:
        raise ProbeError("Favorites pagination exceeded {} pages".format(MAX_PAGES))
    return {"object_id": "FV:2", "total_matches": total, "items": items}


def _service_id(descriptor):
    value = str(descriptor.get("Id") or descriptor.get("id") or "").strip()
    if value:
        return value
    # Sonos encodes a service id in the SA_RINCON type suffix using
    # service_type_id = service_id * 256 + 7.
    service_type = str(descriptor.get("ServiceType") or "").strip()
    match = re.search(r"SA_RINCON(\d+)_", service_type, re.I)
    if match:
        encoded = int(match.group(1))
        if encoded % 256 == 7:
            return str(encoded // 256)
    return ""


def _provider_name(descriptor):
    return str(descriptor.get("Name") or descriptor.get("name") or "").strip()


def _allowed(descriptor):
    name = _provider_name(descriptor).casefold()
    return any(name == candidate for candidate in ALLOWED_SERVICE_NAMES)


def classify_favorite(item, descriptors):
    """Match URI ``sid`` or Sonos SA_RINCON service metadata to descriptor Id."""
    by_id = {_service_id(d): d for d in descriptors if _service_id(d)}
    candidates = set()
    sources = []
    for marker in item.get("service_markers", []):
        values = [marker.get("value", ""), marker.get("text", "")]
        values.extend(marker.get("attributes", {}).values())
        for value in values:
            if value:
                value = str(value).strip()
                if value.isdigit():
                    candidates.add(value)
                    sources.append("exact-service-id")
                match = re.search(r"SA_RINCON(\d+)_", value, re.I)
                if match:
                    encoded = int(match.group(1))
                    if encoded % 256 == 7:
                        sid = str(encoded // 256)
                        candidates.add(sid)
                        sources.append("SA_RINCON-service-type")
    for resource in item.get("resources", []):
        uri = resource.get("uri", "")
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(uri).query)
        for key, values in query.items():
            if key.lower() == "sid":
                for sid in values:
                    candidates.add(sid)
                    sources.append("resource-uri-sid")
    matched = [by_id[sid] for sid in sorted(candidates) if sid in by_id]
    unknown_candidates = sorted(candidates.difference(by_id))
    names = sorted(set(_provider_name(d) for d in matched if _provider_name(d)))
    item["matched_services"] = names
    item["service_id_candidates"] = sorted(candidates)
    item["service_id_sources"] = sorted(set(sources))
    item["unknown_service_ids"] = unknown_candidates
    item["allowed_provider"] = names[0] if len(candidates) == 1 and len(matched) == 1 and _allowed(matched[0]) else None
    item["queueable"] = bool(item["allowed_provider"] and any(
        resource.get("uri", "").strip() for resource in item.get("resources", [])))
    item["queueability"] = "resource-uri-available" if item["queueable"] else "no-resource-uri"
    if item["allowed_provider"]:
        item["provider_status"] = "allowed"
    elif len(candidates) > 1:
        item["provider_status"] = "conflict"
    elif unknown_candidates:
        item["provider_status"] = "unknown-service"
    elif not matched:
        item["provider_status"] = "unmapped" if not matched else "disallowed"
    else:
        item["provider_status"] = "disallowed"
    return item


def run_probe(speaker_ip=None, timeout=DEFAULT_TIMEOUT, discovery_timeout=2.5):
    urls = [description_url_for_ip(speaker_ip)] if speaker_ip else discover(discovery_timeout)
    if not urls:
        raise ProbeError("SSDP found no Sonos speakers; retry with --speaker IP")
    devices, seen = [], set()
    for url in urls:
        try:
            device = fetch_device(url, timeout)
        except ProbeError:
            continue
        identity = device["udn"] or url
        if identity not in seen:
            devices.append(device)
            seen.add(identity)
    if not devices:
        raise ProbeError("No Sonos device descriptions could be read")
    primary = devices[0]
    errors = {}
    try:
        topology = get_topology(primary, timeout)
    except ProbeError as exc:
        topology = None
        errors["topology"] = str(exc)
    try:
        services = list_available_services(primary, timeout)
    except ProbeError as exc:
        services = []
        errors["services"] = str(exc)
    try:
        favorites = browse_favorites(primary, timeout=timeout)
        favorites["items"] = [classify_favorite(item, services) for item in favorites["items"]]
    except ProbeError as exc:
        favorites = None
        errors["favorites"] = str(exc)
    return {
        "read_only": True,
        "devices": [{k: v for k, v in d.items() if k != "services"} for d in devices],
        "topology": topology,
        "available_services": services,
        "favorites": favorites,
        "errors": errors,
        "allowed_favorites": [item for item in (favorites or {}).get("items", [])
                              if item.get("provider_status") == "allowed" and item.get("queueable")],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--speaker", metavar="IP", help="probe this speaker if SSDP is blocked")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help="per-request timeout in seconds (default: %(default)s)")
    parser.add_argument("--discovery-timeout", type=float, default=2.5,
                        help="SSDP collection window in seconds (default: %(default)s)")
    parser.add_argument("--report", metavar="PATH",
                        help="write full local report including URIs and metadata XML")
    args = parser.parse_args(argv)
    try:
        report = run_probe(args.speaker, args.timeout, args.discovery_timeout)
    except ProbeError as exc:
        print("sonos_probe: {}".format(exc), file=sys.stderr)
        return 2
    rendered = json.dumps(report, indent=2, ensure_ascii=False, sort_keys=True)
    if args.report:
        directory = os.path.dirname(os.path.abspath(args.report))
        os.makedirs(directory, exist_ok=True)
        with open(args.report, "w", encoding="utf-8") as handle:
            handle.write(rendered + "\n")
        print("Wrote local Sonos probe report to {}".format(args.report))
    else:
        print(rendered)
    return 0


if __name__ == "__main__":
    sys.exit(main())
