import unittest
import xml.etree.ElementTree as ET
from unittest import mock
from xml.sax.saxutils import escape

from tools import sonos_probe as probe


def browse_response(result_xml, returned, total):
    root = ET.Element("{urn:schemas-xmlsoap-org:soap.v1}Envelope")
    body = ET.SubElement(root, "{urn:schemas-xmlsoap-org:soap.v1}Body")
    response = ET.SubElement(body, "BrowseResponse")
    result = ET.SubElement(response, "BrowseResult")
    ET.SubElement(result, "Result").text = result_xml
    ET.SubElement(result, "NumberReturned").text = str(returned)
    ET.SubElement(result, "TotalMatches").text = str(total)
    ET.SubElement(result, "UpdateID").text = "7"
    return ET.tostring(root, encoding="utf-8")


def favorite(title, uri, service_marker=""):
    safe_title = escape(title)
    safe_uri = escape(uri)
    attr = ('<desc id="{}"/>'.format(service_marker) if service_marker else "")
    return ('<item id="F:{}" parentID="FV:2" restricted="true">'
            '<title>{}</title><res>{}</res>{}</item>'.format(
                safe_title, safe_title, safe_uri, attr))


class ProbeTests(unittest.TestCase):
    def test_soap_envelope_escapes_arguments(self):
        payload = probe.soap_envelope("{urn:test}Browse", {"ObjectID": "A&B <C> \"D\""})
        root = ET.fromstring(payload)
        value = next(node.text for node in root.iter() if probe.local_name(node.tag) == "ObjectID")
        self.assertEqual(value, 'A&B <C> "D"')
        self.assertIn(b"A&amp;B &lt;C&gt;", payload)

    def test_pagination_preserves_uri_metadata_and_requests_next_page(self):
        pages = [
            browse_response('<DIDL-Lite>{}</DIDL-Lite>'.format(
                favorite("Apple <Mix>", "x-rincon-cpcontainer:1", "204")), 1, 2),
            browse_response('<DIDL-Lite>{}</DIDL-Lite>'.format(
                favorite("Station", "x-rincon-mp3radio:2", "254")), 1, 2),
        ]
        device = {"services": {probe.CONTENT_DIRECTORY: "http://speaker:1400/ContentDirectory/Control"}}
        with mock.patch.object(probe, "soap_call", side_effect=[ET.fromstring(p) for p in pages]) as call:
            result = probe.browse_favorites(device, page_size=1)
        self.assertEqual(len(result["items"]), 2)
        self.assertEqual(result["items"][0]["title"], "Apple <Mix>")
        self.assertEqual(result["items"][0]["resources"][0]["uri"], "x-rincon-cpcontainer:1")
        self.assertIn("Apple &lt;Mix&gt;", result["items"][0]["metadata_xml"])
        self.assertEqual(call.call_count, 2)
        self.assertEqual(call.call_args_list[0].args[2], "Browse")
        self.assertEqual(call.call_args_list[0].args[3]["ObjectID"], "FV:2")
        self.assertEqual(call.call_args_list[1].args[3]["StartingIndex"], 1)

    def test_provider_filter_requires_exact_service_metadata(self):
        descriptors = [
            {"Id": "204", "Name": "Apple Music"},
            {"Id": "254", "Name": "Sonos Radio"},
            {"Id": "9", "Name": "Other service"},
        ]
        apple = probe._didl_items('<DIDL-Lite>{}</DIDL-Lite>'.format(
            favorite("Sonos Radio station title", "opaque", "204")))[0]
        radio = probe._didl_items('<DIDL-Lite>{}</DIDL-Lite>'.format(
            favorite("Something", "opaque", "254")))[0]
        unknown = probe._didl_items('<DIDL-Lite>{}</DIDL-Lite>'.format(
            favorite("Apple Music playlist title", "opaque")))[0]
        self.assertEqual(probe.classify_favorite(apple, descriptors)["allowed_provider"], "Apple Music")
        self.assertEqual(probe.classify_favorite(radio, descriptors)["allowed_provider"], "Sonos Radio")
        self.assertIsNone(probe.classify_favorite(unknown, descriptors)["allowed_provider"])

    def test_sonos_uri_sid_and_service_type_encoding_match_descriptors(self):
        descriptors = [
            {"Id": "204", "Name": "Apple Music"},
            {"Id": "303", "Name": "Sonos Radio"},
        ]
        apple = probe._didl_items(
            '<DIDL-Lite><item><title>Any title</title>'
            '<res>x-rincon-cpcontainer:opaque?sid=204&amp;flags=8300</res>'
            '</item></DIDL-Lite>')[0]
        radio = probe._didl_items(
            '<DIDL-Lite><item><title>Any other title</title>'
            '<res>x-sonosapi-radio:opaque?sid=303&amp;sn=4</res>'
            '</item></DIDL-Lite>')[0]
        mismatched = probe._didl_items(
            '<DIDL-Lite><item><title>Apple Music</title>'
            '<res>x-sonosapi-radio:opaque?sid=303</res>'
            '</item></DIDL-Lite>')[0]
        self.assertEqual(probe.classify_favorite(apple, descriptors)["allowed_provider"], "Apple Music")
        self.assertEqual(probe.classify_favorite(radio, descriptors)["allowed_provider"], "Sonos Radio")
        other = {"ServiceType": "SA_RINCON77575_", "Name": "Other service"}
        self.assertEqual(probe._service_id(other), "303")
        self.assertEqual(probe.classify_favorite(mismatched, [other])["provider_status"], "disallowed")

    def test_resmd_descriptor_maps_id_and_unknown_identity_fails_closed(self):
        descriptors = [
            {"Id": "204", "Name": "Apple Music"},
            {"Id": "303", "Name": "Sonos Radio"},
        ]
        item = probe._didl_items(
            '<DIDL-Lite><item><title>Anything</title>'
            '<res>x-rincon-cpcontainer:opaque?sid=204</res>'
            '<resMD>&lt;DIDL-Lite&gt;&lt;item&gt;'
            '&lt;desc id="cdudn"&gt;SA_RINCON52231_X_#Svc52231-0-Token&lt;/desc&gt;'
            '&lt;/item&gt;&lt;/DIDL-Lite&gt;</resMD>'
            '</item></DIDL-Lite>')[0]
        result = probe.classify_favorite(item, descriptors)
        self.assertEqual(result["service_id_candidates"], ["204"])
        self.assertEqual(result["allowed_provider"], "Apple Music")
        self.assertEqual(result["service_id_sources"], ["SA_RINCON-service-type", "resource-uri-sid"])

        unknown = probe._didl_items(
            '<DIDL-Lite><item><title>Apple Music is in the title</title>'
            '<res>x-rincon-cpcontainer:opaque?sid=999</res>'
            '</item></DIDL-Lite>')[0]
        result = probe.classify_favorite(unknown, descriptors)
        self.assertEqual(result["provider_status"], "unknown-service")
        self.assertIsNone(result["allowed_provider"])

    def test_conflicting_provider_ids_are_rejected(self):
        descriptors = [
            {"Id": "204", "Name": "Apple Music"},
            {"Id": "303", "Name": "Sonos Radio"},
        ]
        item = probe._didl_items(
            '<DIDL-Lite><item><title>Any title</title>'
            '<res>x-rincon-cpcontainer:one?sid=204</res>'
            '<res>x-sonosapi-radio:two?sid=303</res>'
            '</item></DIDL-Lite>')[0]
        result = probe.classify_favorite(item, descriptors)
        self.assertEqual(result["provider_status"], "conflict")
        self.assertIsNone(result["allowed_provider"])

    def test_allowed_service_without_resource_uri_is_not_queueable(self):
        item = probe._didl_items(
            '<DIDL-Lite><item><title>Discover Sonos Radio</title>'
            '<res></res><resMD>&lt;DIDL-Lite&gt;&lt;item&gt;'
            '&lt;desc id="cdudn"&gt;SA_RINCON77575_X_#Svc77575-Token&lt;/desc&gt;'
            '&lt;/item&gt;&lt;/DIDL-Lite&gt;</resMD>'
            '</item></DIDL-Lite>')[0]
        result = probe.classify_favorite(item, [{"Id": "303", "Name": "Sonos Radio"}])
        self.assertEqual(result["provider_status"], "allowed")
        self.assertEqual(result["allowed_provider"], "Sonos Radio")
        self.assertFalse(result["queueable"])
        self.assertEqual(result["queueability"], "no-resource-uri")

    def test_malformed_favorites_xml_raises_probe_error(self):
        with self.assertRaisesRegex(probe.ProbeError, "Malformed favorites DIDL XML"):
            probe._didl_items("<DIDL-Lite><item>")

    def test_pagination_count_mismatch_fails_closed(self):
        response = browse_response('<DIDL-Lite>{}</DIDL-Lite>'.format(
            favorite("one", "uri", "204")), 3, 3)
        device = {"services": {probe.CONTENT_DIRECTORY: "http://speaker:1400/control"}}
        with mock.patch.object(probe, "soap_call", return_value=ET.fromstring(response)):
            with self.assertRaisesRegex(probe.ProbeError, "NumberReturned=3 but parsed 1"):
                probe.browse_favorites(device)


if __name__ == "__main__":
    unittest.main()
